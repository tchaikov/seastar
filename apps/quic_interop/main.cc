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
 * QUIC Interop Runner server endpoint. Serves files from a document root
 * over HTTP/3, following the quic-interop-runner container contract
 * (env vars ROLE / TESTCASE / QLOGDIR, certs under /certs, docroot /www,
 * port 443). Unsupported roles/testcases exit 127 so the runner marks
 * them unsupported.
 */

#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/http/file_handler.hh>
#include <seastar/http3/server.hh>

#include <fmt/printf.h>

#include <cstdlib>
#include <set>

using namespace seastar;
namespace bpo = boost::program_options;

namespace {

// Testcases this endpoint supports over HTTP/3.
const std::set<std::string> supported_testcases = {
    "handshake", "transfer", "http3", "multiplexing", "retry", "resumption",
    "zerortt", "chacha20", "keyupdate",
};

std::string env_or(const char* name, const char* def) {
    const char* v = ::getenv(name);
    return v ? std::string(v) : std::string(def);
}

} // anonymous namespace

int main(int ac, char** av) {
    // Honor the interop-runner contract before starting the reactor.
    auto role = env_or("ROLE", "server");
    if (role != "server") {
        fmt::print(stderr, "quic_interop: only ROLE=server is supported\n");
        return 127;
    }
    auto testcase = env_or("TESTCASE", "");
    if (!testcase.empty() && !supported_testcases.contains(testcase)) {
        fmt::print(stderr, "quic_interop: unsupported TESTCASE={}\n", testcase);
        return 127;
    }

    app_template app;
    app.add_options()
        ("cert", bpo::value<std::string>()->default_value("/certs/cert.pem"), "server certificate")
        ("key", bpo::value<std::string>()->default_value("/certs/priv.key"), "server private key")
        ("docroot", bpo::value<std::string>()->default_value("/www"), "document root")
        ("port", bpo::value<uint16_t>()->default_value(443), "UDP port")
        ("retry", bpo::value<bool>()->default_value(false), "force address validation (Retry)");

    // The interop 'retry' testcase requires forced address validation.
    if (testcase == "retry") {
        // Simulate --retry on the command line.
        static std::string retry_arg = "--retry=true";
        static std::vector<char*> argv(av, av + ac);
        argv.push_back(retry_arg.data());
        av = argv.data();
        ac = static_cast<int>(argv.size());
    }

    return app.run(ac, av, [&app] () -> future<int> {
        auto& cfg = app.configuration();
        auto cert = cfg["cert"].as<std::string>();
        auto key = cfg["key"].as<std::string>();
        auto docroot = cfg["docroot"].as<std::string>();
        auto port = cfg["port"].as<uint16_t>();

        // The credentials builder is copyable across shards; each shard
        // builds its own credentials.
        tls::credentials_builder creds;
        co_await creds.set_x509_key_file(cert, key, tls::x509_crt_format::PEM);

        net::quic::listen_options lo;
        lo.require_retry = cfg["retry"].as<bool>();

        // One server per shard, all bound to the same address (the interop
        // runner uses --smp 1, but the code path is the sharded one).
        auto server = std::make_unique<experimental::http3::http3_server_control>();
        co_await server->start();
        // directory_handler concatenates doc_root with the decoded "path"
        // param (whose leading slash get_decoded_param strips), so doc_root
        // must end with a separator.
        if (!docroot.empty() && docroot.back() != '/') {
            docroot += '/';
        }
        co_await server->set_routes([docroot] (httpd::routes& r) {
            r.add(httpd::operation_type::GET, httpd::url("").remainder("path"),
                  new httpd::directory_handler(docroot));
        });
        co_await server->listen(socket_address(ipv4_addr{"0.0.0.0", port}), creds, lo);

        fmt::print("quic_interop: serving {} over h3 on :{} on {} shards\n", docroot, port, smp::count);

        // Run until signalled.
        co_await engine().wait_for_stop(std::chrono::hours(24 * 365));
        co_await server->stop();
        co_return 0;
    });
}
