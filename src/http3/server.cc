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
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/http/common.hh>
#include <seastar/http3/server.hh>
#include <seastar/util/short_streams.hh>

namespace seastar::experimental::http3 {

extern logger h3_log;

namespace internal {

// One HTTP/3 server connection: turns each request stream into a
// httpd::routes dispatch and streams the reply back.
class server_connection : public h3_connection {
    http3_server& _server;
    nghttp3_callbacks _callbacks{};

public:
    server_connection(http3_server& server, quic::connection conn)
        : h3_connection(std::move(conn), /*server=*/true)
        , _server(server) {
        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        if (nghttp3_conn_server_new(&_h3, &server_callbacks(), &settings, nullptr, this) != 0) {
            throw http3_exception(h3_error_code::internal_error, "nghttp3_conn_server_new");
        }
    }

private:
    const nghttp3_callbacks& server_callbacks() {
        _callbacks.begin_headers = on_begin_headers;
        _callbacks.recv_header = on_recv_header;
        _callbacks.end_headers = on_end_headers;
        _callbacks.recv_data = on_recv_data;
        _callbacks.end_stream = on_end_stream;
        _callbacks.stream_close = on_stream_close;
        _callbacks.acked_stream_data = on_acked_stream_data;
        return _callbacks;
    }

    static server_connection& from(void* p) {
        return *static_cast<server_connection*>(p);
    }

    static int on_begin_headers(nghttp3_conn*, int64_t stream_id, void* user, void*) {
        auto& self = from(user);
        if (auto* s = self.find_stream(stream_id)) {
            s->request = std::make_unique<http::request>();
            s->request->_version = "3";
        }
        return 0;
    }

    static int on_recv_header(nghttp3_conn*, int64_t stream_id, int32_t token,
                              nghttp3_rcbuf* name, nghttp3_rcbuf* value, uint8_t, void* user, void*) {
        auto& self = from(user);
        auto* s = self.find_stream(stream_id);
        if (!s || !s->request) {
            return 0;
        }
        apply_request_header(*s->request, token, to_view(name), to_view(value));
        return 0;
    }

    static int on_end_headers(nghttp3_conn*, int64_t stream_id, int /*fin*/, void* user, void*) {
        auto& self = from(user);
        auto* s = self.find_stream(stream_id);
        if (!s || !s->request) {
            return 0;
        }
        s->headers_done = true;
        s->headers_cv.broadcast();
        // Dispatch once headers are known; body (if any) is streamed via
        // the request's content_stream.
        self.dispatch(stream_id);
        return 0;
    }

    static int on_stream_close(nghttp3_conn*, int64_t stream_id, uint64_t, void* user, void*) {
        auto& self = from(user);
        self.release_stream(stream_id);
        return 0;
    }

    // Pull-model data reader for the response body.
    void dispatch(int64_t stream_id) {
        (void)do_dispatch(stream_id).handle_exception([] (std::exception_ptr ep) {
            h3_log.debug("h3 request dispatch failed: {}", ep);
        }).finally([self = shared_from_this()] {});
    }

    future<> do_dispatch(int64_t stream_id) {
        auto hold = _gate.hold();
        auto* s0 = find_stream(stream_id);
        if (!s0 || !s0->request) {
            co_return;
        }
        auto req = std::move(s0->request);

        // The request body: reads grant flow-control credit, so a slow
        // consumer backpressures the peer. In streaming mode the handler
        // reads the body itself via request::content_stream; otherwise
        // buffer it here (like the TCP server's default).
        auto body_in = input_stream<char>(data_source(std::make_unique<body_source>(shared_from_this(), stream_id)));
        if (_server.get_content_streaming()) {
            req->content_stream = &body_in;
        } else {
            try {
                req->content = co_await util::read_entire_stream_contiguous(body_in);
            } catch (...) {
                co_return; // request aborted mid-body
            }
            req->content_length = req->content.size();
        }

        // Route the request.
        auto rep = std::make_unique<http::reply>();
        auto url = req->_url;
        std::unique_ptr<http::reply> result;
        try {
            result = co_await _server._routes.handle(url, std::move(req), std::move(rep));
        } catch (...) {
            result = std::make_unique<http::reply>();
            result->set_status(http::reply::status_type::internal_server_error);
        }
        ++_server._requests_served;

        // In streaming mode the handler may not have consumed the whole
        // body; drain the rest so its flow-control credit returns.
        if (_server.get_content_streaming()) {
            try {
                co_await util::skip_entire_stream(body_in);
            } catch (...) {
            }
        }
        co_await body_in.close();

        auto* s = find_stream(stream_id);
        if (!s) {
            co_return;
        }

        // Submit the response headers with a pull-model data reader now;
        // the body streams in concurrently below (bounded memory).
        // `storage` must outlive the submit call (nghttp3 references the
        // header strings until it encodes them), which it does — it lives
        // for the whole coroutine.
        std::vector<sstring> storage;
        auto nva = reply_to_nva(*result, storage);
        nghttp3_data_reader dr{read_body_data};
        if (nghttp3_conn_submit_response(_h3, stream_id, nva.data(), nva.size(), &dr) != 0) {
            h3_log.debug("nghttp3_conn_submit_response failed on stream {}", stream_id);
            co_return;
        }
        kick_send();

        // Produce the reply body.
        if (!result->_content.empty()) {
            // Inline content (small handler responses): one chunk. The
            // copy here is negligible; the large-body path below is
            // zero-copy.
            auto len = result->_content.size();
            s->send_body.emplace_back(result->_content.data(), len);
            s->send_queued += len;
            s->send_eof = true;
            resume_stream(stream_id);
        } else if (result->has_body_writer()) {
            // Stream the writer's output through a watermarked queue so a
            // large body (e.g. a big file) uses bounded memory, with the
            // buffers moved (not copied) into the response send queue.
            auto writer = result->release_body_writer();
            auto out = output_stream<char>(
                data_sink(std::make_unique<body_sink>(shared_from_this(), stream_id)), 64 * 1024);
            try {
                co_await writer(std::move(out));
            } catch (...) {
                h3_log.debug("h3 body writer failed on stream {}: {}", stream_id, std::current_exception());
                if (auto* st = find_stream(stream_id)) {
                    st->send_eof = true;
                    resume_stream(stream_id);
                }
            }
        } else {
            s->send_eof = true;
            resume_stream(stream_id);
        }
    }

};

} // namespace internal

http3_server::~http3_server() = default;

socket_address http3_server::local_address() const {
    return _server ? _server->local_address() : socket_address{};
}

future<> http3_server::listen(socket_address addr, shared_ptr<tls::server_credentials> creds,
                              net::quic::listen_options options) {
    // Ensure the "h3" ALPN is advertised.
    creds->set_alpn_protocols({"h3"});
    _server.emplace(net::quic::listen(addr, std::move(creds), std::move(options)));
    (void)accept_loop().handle_exception([] (std::exception_ptr ep) {
        h3_log.debug("h3 server accept loop failed: {}", ep);
    });
    return make_ready_future<>();
}

future<> http3_server::accept_loop() {
    auto hold = _gate.hold();
    while (!_stopping) {
        net::quic::connection conn;
        try {
            conn = co_await _server->accept();
        } catch (...) {
            break;
        }
        auto sc = make_shared<internal::server_connection>(*this, std::move(conn));
        _connections.push_back(sc);
        (void)sc->start().handle_exception([] (std::exception_ptr ep) {
            h3_log.debug("h3 server connection setup failed: {}", ep);
        });
        // Drop our reference once the connection fully closes, so a
        // long-lived server does not accumulate dead connections. The gate
        // keeps this continuation from outliving the server.
        (void)with_gate(_gate, [this, sc] {
            return sc->closed().then([this, sc] {
                if (!_stopping) {
                    std::erase(_connections, sc);
                }
            });
        });
    }
}

future<> http3_server::stop() {
    _stopping = true;
    if (_server) {
        co_await _server->stop();
    }
    co_await coroutine::parallel_for_each(_connections, [] (auto& c) -> future<> {
        co_await c->close();
    });
    _connections.clear();
    co_await _gate.close();
}

// http3_server_control

http3_server_control::http3_server_control()
    : _server(std::make_unique<sharded<http3_server>>()) {
}

future<> http3_server_control::start() {
    return _server->start();
}

future<> http3_server_control::stop() noexcept {
    return _server->stop();
}

future<> http3_server_control::set_routes(std::function<void(httpd::routes&)> fun) {
    return _server->invoke_on_all([fun = std::move(fun)] (http3_server& s) {
        fun(s._routes);
    });
}

future<> http3_server_control::listen(socket_address addr, tls::credentials_builder builder,
                                      net::quic::listen_options options) {
    // The builder is copyable across shards; each shard builds (and owns)
    // its own server_credentials, since credentials objects are
    // shard-affine and must not be shared.
    return _server->invoke_on_all([addr, builder = std::move(builder), options] (http3_server& s) {
        return s.listen(addr, builder.build_server_credentials(), options);
    });
}

} // namespace seastar::experimental::http3
