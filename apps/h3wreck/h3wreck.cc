/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2026 Kefu Chai ( tchaikov@gmail.com )
 *
 * An HTTP/3 load generator, in the spirit of seawreck. Drives concurrent
 * requests over one QUIC connection per shard and reports throughput and
 * latency percentiles.
 */

#include <seastar/core/abort_source.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/timer.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/http3/client.hh>
#include <seastar/net/dns.hh>
#include <seastar/util/short_streams.hh>

#include <fmt/printf.h>

#include <algorithm>
#include <chrono>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;
namespace bpo = boost::program_options;

using hr_clock = std::chrono::steady_clock;

// A latency histogram with fixed logarithmic buckets (microseconds).
struct latency_histogram {
    static constexpr size_t bucket_count = 40; // ~1us .. ~10s, 4 buckets/decade
    std::array<uint64_t, bucket_count> buckets{};
    uint64_t count = 0;
    uint64_t sum_us = 0;
    uint64_t max_us = 0;

    static size_t bucket_of(uint64_t us) {
        if (us == 0) {
            return 0;
        }
        auto b = static_cast<size_t>(4.0 * std::log10(static_cast<double>(us)));
        return std::min(b, bucket_count - 1);
    }
    void record(uint64_t us) {
        buckets[bucket_of(us)]++;
        ++count;
        sum_us += us;
        max_us = std::max(max_us, us);
    }
    void merge(const latency_histogram& o) {
        for (size_t i = 0; i < bucket_count; ++i) {
            buckets[i] += o.buckets[i];
        }
        count += o.count;
        sum_us += o.sum_us;
        max_us = std::max(max_us, o.max_us);
    }
    // Approximate percentile (upper bucket edge), in microseconds.
    double percentile(double p) const {
        uint64_t target = static_cast<uint64_t>(p * count);
        uint64_t acc = 0;
        for (size_t i = 0; i < bucket_count; ++i) {
            acc += buckets[i];
            if (acc >= target) {
                return std::pow(10.0, (i + 1) / 4.0);
            }
        }
        return static_cast<double>(max_us);
    }
};

// Per-shard load generator.
class h3_load {
    socket_address _addr;
    sstring _host;
    sstring _path;
    unsigned _streams;      // concurrent in-flight requests
    unsigned _reqs;         // per-shard request budget (0 = duration mode)
    hr_clock::time_point _deadline;
    unsigned _req_timeout_ms; // per-request timeout (0 = no timeout)
    shared_ptr<tls::certificate_credentials> _creds;
    std::unique_ptr<experimental::http3::client> _client;
    latency_histogram _hist;
    uint64_t _errors = 0;
    uint64_t _bytes = 0;
    uint64_t _done = 0;

public:
    h3_load(socket_address addr, sstring host, sstring path, unsigned streams, unsigned reqs,
            unsigned duration_s, unsigned req_timeout_ms)
        : _addr(addr), _host(std::move(host)), _path(std::move(path))
        , _streams(streams), _reqs(reqs)
        , _deadline(hr_clock::now() + std::chrono::seconds(duration_s))
        , _req_timeout_ms(req_timeout_ms) {
    }

    future<> setup() {
        _creds = make_shared<tls::certificate_credentials>();
        co_await _creds->set_system_trust();
        // For benchmarking we usually target a self-signed server.
        _creds->set_enable_certificate_verification(false);
        _client = std::make_unique<experimental::http3::client>(_addr, _creds, _host);
    }

    bool more() const {
        return _reqs ? _done < _reqs : hr_clock::now() < _deadline;
    }

    future<> run() {
        co_await coroutine::parallel_for_each(std::views::iota(0u, _streams), [this] (unsigned) -> future<> {
            while (more()) {
                auto start = hr_clock::now();
                http::request req;
                req._method = "GET";
                req._url = _path;
                // Bound each request so a wedged stream cannot hang the
                // run; a fired timer cancels the request, counted as an
                // error below.
                abort_source as;
                timer<> req_timer([&as] { as.request_abort(); });
                if (_req_timeout_ms) {
                    req_timer.arm(std::chrono::milliseconds(_req_timeout_ms));
                }
                try {
                    co_await _client->make_request(std::move(req),
                        [this] (const http::reply&, input_stream<char>&& body) -> future<> {
                            auto data = co_await util::read_entire_stream_contiguous(body);
                            _bytes += data.size();
                        }, std::nullopt, &as);
                    auto us = std::chrono::duration_cast<std::chrono::microseconds>(hr_clock::now() - start).count();
                    _hist.record(us);
                } catch (...) {
                    ++_errors;
                }
                req_timer.cancel();
                ++_done;
            }
        });
    }

    future<> stop() {
        if (_client) {
            co_await _client->close();
        }
    }

    // Copyable snapshot of a shard's results, for map_reduce aggregation.
    struct stats {
        latency_histogram hist;
        uint64_t reqs = 0;
        uint64_t errors = 0;
        uint64_t bytes = 0;
        stats& operator+=(const stats& o) {
            hist.merge(o.hist);
            reqs += o.reqs;
            errors += o.errors;
            bytes += o.bytes;
            return *this;
        }
    };
    stats snapshot() const {
        return stats{_hist, _done, _errors, _bytes};
    }
};

int main(int ac, char** av) {
    app_template app;
    app.add_options()
        ("server,s", bpo::value<std::string>()->default_value("127.0.0.1:443"), "server address host:port")
        ("host", bpo::value<std::string>()->default_value(""), "authority/SNI (defaults to server host)")
        ("path", bpo::value<std::string>()->default_value("/"), "request path")
        ("streams,m", bpo::value<unsigned>()->default_value(16), "concurrent streams per shard")
        ("reqs,r", bpo::value<unsigned>()->default_value(0), "total requests per shard (0 = duration mode)")
        ("duration,d", bpo::value<unsigned>()->default_value(10), "test duration in seconds")
        ("request-timeout", bpo::value<unsigned>()->default_value(30'000),
         "per-request timeout in milliseconds; timed-out requests are cancelled and counted as errors (0 = no timeout)");

    return app.run(ac, av, [&app] () -> future<int> {
        auto& cfg = app.configuration();
        auto server = cfg["server"].as<std::string>();
        auto host = cfg["host"].as<std::string>();
        auto path = cfg["path"].as<std::string>();
        auto streams = cfg["streams"].as<unsigned>();
        auto reqs = cfg["reqs"].as<unsigned>();
        auto duration = cfg["duration"].as<unsigned>();
        auto req_timeout = cfg["request-timeout"].as<unsigned>();

        auto colon = server.rfind(':');
        auto host_part = server.substr(0, colon);
        auto port = static_cast<uint16_t>(std::stoi(server.substr(colon + 1)));
        if (host.empty()) {
            host = host_part;
        }
        auto resolved = co_await net::dns::resolve_name(host_part);
        auto addr = socket_address(resolved, port);

        auto gens = std::make_unique<sharded<h3_load>>();
        co_await gens->start(addr, sstring(host), sstring(path), streams, reqs, duration, req_timeout);
        co_await gens->invoke_on_all(&h3_load::setup);

        fmt::print("========== h3wreck ==========\n");
        fmt::print("Server: {}  path: {}  shards: {}  streams/shard: {}\n",
                   server, path, smp::count, streams);

        auto started = hr_clock::now();
        co_await gens->invoke_on_all(&h3_load::run);
        auto elapsed = std::chrono::duration<double>(hr_clock::now() - started).count();

        // Aggregate per-shard stats (each snapshot is copied to this shard).
        auto agg = co_await gens->map_reduce0(
            [] (const h3_load& g) { return g.snapshot(); },
            h3_load::stats{},
            [] (h3_load::stats a, h3_load::stats b) { a += b; return a; });

        co_await gens->invoke_on_all(&h3_load::stop);
        co_await gens->stop();

        const auto& hist = agg.hist;
        fmt::print("Requests: {}  errors: {}  bytes: {}\n", agg.reqs, agg.errors, agg.bytes);
        fmt::print("Duration: {:.2f}s  throughput: {:.0f} req/s  {:.2f} MB/s\n",
                   elapsed, agg.reqs / elapsed, agg.bytes / elapsed / (1 << 20));
        if (hist.count) {
            fmt::print("Latency (us): mean {:.0f}  p50 {:.0f}  p90 {:.0f}  p99 {:.0f}  p99.9 {:.0f}  max {}\n",
                       double(hist.sum_us) / hist.count, hist.percentile(0.50), hist.percentile(0.90),
                       hist.percentile(0.99), hist.percentile(0.999), hist.max_us);
        }
        fmt::print("=============================\n");
        co_return 0;
    });
}
