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

#include <seastar/testing/test_case.hh>

#include <seastar/core/abort_source.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/core/seastar.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/http/file_handler.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http/routes.hh>
#include <seastar/http3/client.hh>
#include <seastar/http3/server.hh>
#include <seastar/net/quic/quic.hh>
#include <seastar/util/short_streams.hh>
#include <seastar/util/tmp_file.hh>

#include "quic_tls_helpers.hh"

#include <ranges>

using namespace seastar;
using namespace seastar::experimental;
using quic_test_helpers::make_server_creds;
using quic_test_helpers::make_client_creds;

SEASTAR_TEST_CASE(test_http3_get) {
    auto server_creds = co_await make_server_creds();
    auto client_creds = co_await make_client_creds();

    http3::http3_server server;
    server._routes.put(httpd::GET, "/hello",
        new httpd::function_handler([] (httpd::const_req req) {
            return sstring("world");
        }, "txt"));
    co_await server.listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds);
    auto addr = server.local_address();

    http3::client client(addr, client_creds, "test.scylladb.org");

    http::request req;
    req._method = "GET";
    req._url = "/hello";
    sstring got;
    co_await client.make_request(std::move(req),
        [&got] (const http::reply& rep, input_stream<char>&& body) -> future<> {
            BOOST_REQUIRE(rep._status == http::reply::status_type::ok);
            got = co_await util::read_entire_stream_contiguous(body);
        });
    BOOST_REQUIRE_EQUAL(got, "world");

    co_await client.close();
    co_await server.stop();
}

SEASTAR_TEST_CASE(test_http3_404) {
    auto server_creds = co_await make_server_creds();
    auto client_creds = co_await make_client_creds();

    http3::http3_server server;
    server._routes.put(httpd::GET, "/exists",
        new httpd::function_handler([] (httpd::const_req) {
            return sstring("ok");
        }, "txt"));
    co_await server.listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds);
    auto addr = server.local_address();

    http3::client client(addr, client_creds, "test.scylladb.org");

    http::request req;
    req._method = "GET";
    req._url = "/missing";
    http::reply::status_type status{};
    co_await client.make_request(std::move(req),
        [&status] (const http::reply& rep, input_stream<char>&& body) -> future<> {
            status = rep._status;
            co_await util::skip_entire_stream(body);
        });
    BOOST_REQUIRE(status == http::reply::status_type::not_found);

    co_await client.close();
    co_await server.stop();
}

SEASTAR_TEST_CASE(test_http3_streamed_body) {
    auto server_creds = co_await make_server_creds();
    auto client_creds = co_await make_client_creds();

    http3::http3_server server;
    // Reply via a streaming body writer (as the file handler does), with a
    // body large enough to span many DATA frames.
    server._routes.put(httpd::GET, "/stream",
        new httpd::function_handler([] (httpd::const_req, http::reply& rep) {
            rep.write_body("txt", [] (output_stream<char>&& out) -> future<> {
                for (int i = 0; i < 200000; ++i) {
                    co_await out.write(sstring("0123456789"));
                }
                co_await out.close();
            });
            return sstring();
        }, "txt"));
    co_await server.listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds);
    auto addr = server.local_address();

    http3::client client(addr, client_creds, "test.scylladb.org");
    http::request req;
    req._method = "GET";
    req._url = "/stream";
    size_t got = 0;
    co_await client.make_request(std::move(req),
        [&got] (const http::reply&, input_stream<char>&& body) -> future<> {
            auto data = co_await util::read_entire_stream_contiguous(body);
            got = data.size();
        });
    BOOST_REQUIRE_EQUAL(got, 2000000u);

    co_await client.close();
    co_await server.stop();
}

SEASTAR_TEST_CASE(test_http3_post_echo_streaming) {
    auto server_creds = co_await make_server_creds();
    auto client_creds = co_await make_client_creds();

    // An async handler that streams the request body in (via
    // content_stream) and echoes it back through a streaming body writer,
    // so a large upload+download round-trips in bounded memory.
    class echo_handler : public httpd::handler_base {
        future<std::unique_ptr<http::reply>> handle(const sstring&,
                std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) override {
            auto body = co_await util::read_entire_stream_contiguous(*req->content_stream);
            rep->write_body("txt", [body = std::move(body)] (output_stream<char>&& out) mutable -> future<> {
                co_await out.write(body);
                co_await out.close();
            });
            co_return std::move(rep);
        }
    };

    http3::http3_server server;
    server.set_content_streaming(true);
    server._routes.put(httpd::POST, "/echo", new echo_handler());
    co_await server.listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds);
    auto addr = server.local_address();

    http3::client client(addr, client_creds, "test.scylladb.org");
    http::request req;
    req._method = "POST";
    req._url = "/echo";
    constexpr size_t chunks = 200;         // 200 x 10 KiB = 2 MiB upload
    req.write_body("txt", [] (output_stream<char>&& out) -> future<> {
        auto chunk = uninitialized_string(10240);
        std::fill(chunk.begin(), chunk.end(), 'e');
        for (size_t i = 0; i < chunks; ++i) {
            co_await out.write(chunk);
        }
        co_await out.close();
    });
    size_t got = 0;
    co_await client.make_request(std::move(req),
        [&got] (const http::reply& rep, input_stream<char>&& body) -> future<> {
            BOOST_REQUIRE(rep._status == http::reply::status_type::ok);
            auto data = co_await util::read_entire_stream_contiguous(body);
            got = data.size();
        });
    BOOST_REQUIRE_EQUAL(got, chunks * 10240);

    co_await client.close();
    co_await server.stop();
}

SEASTAR_TEST_CASE(test_http3_request_abort) {
    auto server_creds = co_await make_server_creds();
    auto client_creds = co_await make_client_creds();

    http3::http3_server server;
    // A response that stalls mid-body, so the abort fires while the client
    // is reading.
    server._routes.put(httpd::GET, "/stall",
        new httpd::function_handler([] (httpd::const_req, http::reply& rep) {
            rep.write_body("txt", [] (output_stream<char>&& out) -> future<> {
                co_await out.write(sstring("partial"));
                co_await out.flush();
                co_await seastar::sleep(std::chrono::seconds(2));
                co_await out.write(sstring("rest"));
                co_await out.close();
            });
            return sstring();
        }, "txt"));
    server._routes.put(httpd::GET, "/ok",
        new httpd::function_handler([] (httpd::const_req) {
            return sstring("fine");
        }, "txt"));
    co_await server.listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds);
    auto addr = server.local_address();

    http3::client client(addr, client_creds, "test.scylladb.org");

    // Abort the stalled request shortly after it starts.
    abort_source as;
    timer<> abort_timer([&as] { as.request_abort(); });
    abort_timer.arm(std::chrono::milliseconds(200));
    http::request req;
    req._method = "GET";
    req._url = "/stall";
    bool failed = false;
    auto start = std::chrono::steady_clock::now();
    try {
        co_await client.make_request(std::move(req),
            [] (const http::reply&, input_stream<char>&& body) -> future<> {
                co_await util::skip_entire_stream(body);
            }, std::nullopt, &as);
    } catch (...) {
        failed = true;
    }
    abort_timer.cancel();
    BOOST_REQUIRE(failed);
    // The abort must not wait out the server's 2s stall.
    BOOST_REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));

    // The connection stays usable for subsequent requests.
    http::request req2;
    req2._method = "GET";
    req2._url = "/ok";
    sstring got;
    co_await client.make_request(std::move(req2),
        [&got] (const http::reply&, input_stream<char>&& body) -> future<> {
            got = co_await util::read_entire_stream_contiguous(body);
        });
    BOOST_REQUIRE_EQUAL(got, "fine");

    co_await client.close();
    co_await server.stop();
}

// Exercise the interop server's core: serve a file from disk over HTTP/3
// via httpd::directory_handler.
SEASTAR_TEST_CASE(test_http3_file_server) {
    co_await tmp_dir::do_with([] (tmp_dir& td) -> future<> {
        auto server_creds = co_await make_server_creds();
        auto client_creds = co_await make_client_creds();

        // Write a file into the doc root.
        auto docroot = td.get_path().native();
        sstring content(4096, 'z');
        content += "\ntrailer";
        auto f = co_await open_file_dma((td.get_path() / "data.txt").native(),
                                        open_flags::wo | open_flags::create | open_flags::truncate);
        auto out = co_await make_file_output_stream(f);
        co_await out.write(content);
        co_await out.close();

        http3::http3_server server;
        // directory_handler requires a trailing separator on the doc root
        // (get_decoded_param strips the captured leading slash).
        server._routes.add(httpd::operation_type::GET, httpd::url("").remainder("path"),
                           new httpd::directory_handler(docroot + "/"));
        co_await server.listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds);
        auto addr = server.local_address();

        http3::client client(addr, client_creds, "test.scylladb.org");
        http::request req;
        req._method = "GET";
        req._url = "/data.txt";
        http::reply::status_type status{};
        sstring got;
        co_await client.make_request(std::move(req),
            [&] (const http::reply& rep, input_stream<char>&& body) -> future<> {
                status = rep._status;
                got = co_await util::read_entire_stream_contiguous(body);
            });
        BOOST_REQUIRE(status == http::reply::status_type::ok);
        BOOST_REQUIRE_EQUAL(got, content);

        co_await client.close();
        co_await server.stop();
    });
}

SEASTAR_TEST_CASE(test_http3_concurrent_requests) {
    auto server_creds = co_await make_server_creds();
    auto client_creds = co_await make_client_creds();

    http3::http3_server server;
    // Echo back a request header so each concurrent response can be matched
    // to its request (also exercises request-header round-tripping).
    server._routes.put(httpd::GET, "/echo",
        new httpd::function_handler([] (httpd::const_req req) {
            return "echo:" + req.get_header("x-req-id");
        }, "txt"));
    co_await server.listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds);
    auto addr = server.local_address();

    http3::client client(addr, client_creds, "test.scylladb.org");

    constexpr int n = 32;
    co_await coroutine::parallel_for_each(std::views::iota(0, n), [&] (int i) -> future<> {
        http::request req;
        req._method = "GET";
        req._url = "/echo";
        req._headers["x-req-id"] = format("{}", i);
        sstring got;
        co_await client.make_request(std::move(req),
            [&got] (const http::reply&, input_stream<char>&& body) -> future<> {
                got = co_await util::read_entire_stream_contiguous(body);
            });
        BOOST_REQUIRE_EQUAL(got, format("echo:{}", i));
    });

    co_await client.close();
    co_await server.stop();
}

SEASTAR_TEST_CASE(test_http3_malformed_frame_does_not_deadlock_stop) {
    // A SETTINGS frame is only valid on the control stream (RFC 9114
    // §7.2.4); one arriving on a request stream is a protocol error nghttp3
    // rejects at parse time. That drives the server's rx_pump into its
    // error path, which used to call close() while still holding the h3
    // connection's own gate — deadlocking forever against _gate.close().
    // This test's real assertion is that server.stop() returns at all.
    auto server_creds = co_await make_server_creds();
    auto client_creds = co_await make_client_creds();

    http3::http3_server server;
    server._routes.put(httpd::GET, "/ok",
        new httpd::function_handler([] (httpd::const_req) {
            return sstring("ok");
        }, "txt"));
    co_await server.listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds);
    auto addr = server.local_address();

    // A raw QUIC connection (bypassing the HTTP/3 client, which would never
    // construct a malformed frame) so we can hand-craft the bad bytes.
    net::quic::connect_options opts;
    opts.server_name = "test.scylladb.org";
    opts.alpn_protocols = {"h3"};
    auto conn = co_await net::quic::connect(addr, client_creds, std::move(opts));
    auto s = co_await conn.open_stream(net::quic::stream_kind::bidirectional);
    auto out = s.output();
    // varint(type=SETTINGS=0x04) varint(length=0)
    co_await out.write(sstring("\x04\x00", 2));
    co_await out.close();

    // Bounded wait: before the fix this line hung forever.
    co_await with_timeout(std::chrono::steady_clock::now() + std::chrono::seconds(10),
                          server.stop());
    co_await conn.close();
}
