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
 */

#include "h3_connection.hh"
#include "net/quic/connection.hh"
#include "h3_common.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/http3/client.hh>

#include <utility>

namespace seastar::experimental::http3 {

extern logger h3_log;

namespace internal {

// One HTTP/3 client connection: submits requests as bidirectional streams
// and reassembles responses.
class client_connection : public h3_connection {
    sstring _host;
    nghttp3_callbacks _callbacks{};

public:
    client_connection(quic::connection conn, sstring host)
        : h3_connection(std::move(conn), /*server=*/false)
        , _host(std::move(host)) {
        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        if (nghttp3_conn_client_new(&_h3, &client_callbacks(), &settings, nullptr, this) != 0) {
            throw http3_exception(h3_error_code::internal_error, "nghttp3_conn_client_new");
        }
    }

    // Sends a request; resolves with the response reply and hands the
    // caller a stream over the body. When as aborts, the request stream is
    // cancelled and every wait below fails via the aborted flag.
    future<> request(http::request req, client::reply_handler handle,
                     std::optional<http::reply::status_type> expected, abort_source* as) {
        auto hold = _gate.hold();
        if (as) {
            as->check();
        }
        auto qs = co_await _conn.open_stream(quic::stream_kind::bidirectional);
        auto id = qs.id();
        auto& s = track_stream(id, quic::internal::api_access::impl(qs));
        s.reply = std::make_unique<http::reply>();

        // The subscription lives for the whole request; the gate hold keeps
        // this connection alive for at least as long.
        optimized_optional<abort_source::subscription> abort_sub;
        if (as) {
            abort_sub = as->subscribe([this, id] () noexcept {
                abort_request(id);
            });
            if (!abort_sub) {
                abort_request(id);
                as->check();
            }
        }

        std::vector<sstring> storage;
        auto nva = request_to_nva(req, _host, storage);
        // A request body (write_body() or inline content) is served to
        // nghttp3 by the shared pull-model reader over send_body.
        bool has_body = bool(req.body_writer) || !req.content.empty();
        nghttp3_data_reader dr{read_body_data};
        if (nghttp3_conn_submit_request(_h3, id, nva.data(), nva.size(), has_body ? &dr : nullptr, this) != 0) {
            throw http3_exception(h3_error_code::internal_error, "nghttp3_conn_submit_request");
        }
        start_rx_pump(s);
        kick_send();

        std::exception_ptr ex;
        try {
            if (req.body_writer) {
                // Stream the body through the watermarked sink (bounded
                // memory); the writer closes the stream, marking EOF.
                auto body_out = output_stream<char>(
                    data_sink(std::make_unique<body_sink>(shared_from_this(), id)), 64 * 1024);
                auto writer = std::move(req.body_writer);
                co_await writer(std::move(body_out));
            } else if (!req.content.empty()) {
                auto* st = find_stream(id);
                if (st) {
                    st->send_queued += req.content.size();
                    st->send_body.emplace_back(req.content.data(), req.content.size());
                    st->send_eof = true;
                    resume_stream(id);
                }
            }

            // Wait for the response headers.
            {
                auto* st = find_stream(id);
                while (st && !st->headers_done) {
                    co_await st->headers_cv.when();
                    st = find_stream(id);
                }
                if (!st || st->aborted) {
                    throw http3_exception(h3_error_code::request_cancelled, "request aborted");
                }
            }

            auto* st = find_stream(id);
            auto reply = std::move(st->reply);
            if (expected && reply->_status != *expected) {
                throw http3_exception(make_error_code(h3_error_code::message_error),
                                      format("unexpected status {}", static_cast<int>(reply->_status)));
            }

            auto body = input_stream<char>(data_source(std::make_unique<body_source>(shared_from_this(), id)));
            co_await handle(*reply, std::move(body));
        } catch (...) {
            ex = std::current_exception();
        }
        // Forget the stream (returning unconsumed flow-control credit)
        // whether the request succeeded, failed or was aborted.
        release_stream(id);
        if (ex) {
            std::rethrow_exception(ex);
        }
    }

private:

    const nghttp3_callbacks& client_callbacks() {
        _callbacks.begin_headers = on_begin_headers;
        _callbacks.recv_header = on_recv_header;
        _callbacks.end_headers = on_end_headers;
        _callbacks.recv_data = on_recv_data;
        _callbacks.end_stream = on_end_stream;
        _callbacks.stream_close = on_stream_close;
        _callbacks.acked_stream_data = on_acked_stream_data;
        return _callbacks;
    }

    // Cancels an in-flight request: resets both directions of the stream
    // with H3_REQUEST_CANCELLED and wakes every waiter, which then fail
    // via the aborted flag. Safe to call at any point of the request.
    void abort_request(int64_t id) noexcept {
        auto* s = find_stream(id);
        if (!s || s->aborted) {
            return;
        }
        s->aborted = true;
        auto code = quic::application_error_code{std::to_underlying(h3_error_code::request_cancelled)};
        (void)s->qstream->reset_write(code);
        (void)s->qstream->stop_sending(code);
        s->headers_done = true;
        s->headers_cv.broadcast();
        s->recv_cv.broadcast();
    }

    static client_connection& from(void* p) {
        return *static_cast<client_connection*>(p);
    }

    static int on_begin_headers(nghttp3_conn*, int64_t stream_id, void* user, void*) {
        auto& self = from(user);
        if (auto* s = self.find_stream(stream_id); s && !s->reply) {
            s->reply = std::make_unique<http::reply>();
        }
        return 0;
    }

    static int on_recv_header(nghttp3_conn*, int64_t stream_id, int32_t token,
                              nghttp3_rcbuf* name, nghttp3_rcbuf* value, uint8_t, void* user, void*) {
        auto& self = from(user);
        if (auto* s = self.find_stream(stream_id); s && s->reply) {
            apply_reply_header(*s->reply, token, to_view(name), to_view(value));
        }
        return 0;
    }

    static int on_end_headers(nghttp3_conn*, int64_t stream_id, int /*fin*/, void* user, void*) {
        auto& self = from(user);
        if (auto* s = self.find_stream(stream_id)) {
            s->headers_done = true;
            s->headers_cv.broadcast();
        }
        return 0;
    }

    static int on_stream_close(nghttp3_conn*, int64_t stream_id, uint64_t, void* user, void*) {
        auto& self = from(user);
        if (auto* s = self.find_stream(stream_id)) {
            s->recv_eof = true;
            s->recv_cv.broadcast();
            s->headers_done = true;
            s->headers_cv.broadcast();
        }
        return 0;
    }
};

} // namespace internal

client::client(socket_address addr, shared_ptr<tls::certificate_credentials> creds, sstring host)
    : _addr(addr), _creds(std::move(creds)), _host(std::move(host)) {
}

client::~client() = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;

future<> client::ensure_connected() {
    if (_conn) {
        co_return;
    }
    if (_connecting) {
        co_await _connecting->get_future();
        co_return;
    }
    promise<> p;
    _connecting = p.get_future();
    try {
        net::quic::connect_options opts;
        opts.server_name = _host;
        opts.alpn_protocols = {"h3"};
        auto qconn = co_await net::quic::connect(_addr, _creds, std::move(opts));
        auto cc = make_shared<internal::client_connection>(std::move(qconn), _host);
        co_await cc->start();
        _conn = std::move(cc);
        p.set_value();
    } catch (...) {
        _connecting.reset();
        p.set_exception(std::current_exception());
        throw;
    }
}

future<> client::make_request(http::request req, reply_handler handle,
                              std::optional<http::reply::status_type> expected, abort_source* as) {
    co_await ensure_connected();
    co_await _conn->request(std::move(req), std::move(handle), expected, as);
}

future<> client::close() noexcept {
    if (_conn) {
        auto c = std::move(_conn);
        co_await c->close();
    }
}

} // namespace seastar::experimental::http3
