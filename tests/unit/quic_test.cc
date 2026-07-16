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
#include <seastar/testing/thread_test_case.hh>

#include <seastar/core/reactor.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/net/quic/quic.hh>
#include <seastar/net/tls.hh>

// Internal (non-public-API) header, reachable via the test include path,
// giving access to quic_connection_impl::break_channel_for_testing() —
// see its declaration for why a regression test needs it.
#include "net/quic/connection.hh"
#include <seastar/util/short_streams.hh>

#include <ranges>

#include <fmt/ranges.h>

#include "quic_tls_helpers.hh"

using namespace seastar;
using namespace seastar::net;
using quic_test_helpers::make_server_creds;
using quic_test_helpers::make_client_creds;

static quic::connect_options client_options(std::vector<sstring> alpn) {
    quic::connect_options opts;
    opts.server_name = "test.scylladb.org";
    opts.alpn_protocols = std::move(alpn);
    // Loopback: keep the idle timeout generous but not infinite.
    opts.config.max_idle_timeout = std::chrono::seconds(10);
    return opts;
}

// Reads a whole QUIC stream into a string.
static future<sstring> read_all(quic::stream& s) {
    auto in = s.input();
    auto str = co_await util::read_entire_stream_contiguous(in);
    co_await in.close();
    co_return str;
}

// A connected client/server pair. The server-side connection is exposed
// as server_conn; call start_echo() to run an echo server that drains
// each accepted stream (echoing bidi streams, sinking uni streams).
struct connected_pair {
    quic::server server;
    quic::connection client;
    quic::connection server_conn;
    gate echo_gate;
    future<> echo_loop = make_ready_future<>();

    void start_echo() {
        echo_loop = [] (quic::connection& conn, gate& g) -> future<> {
            while (true) {
                quic::stream s;
                try {
                    s = co_await conn.accept_stream();
                } catch (...) {
                    co_return;
                }
                (void)with_gate(g, [s = std::move(s)] () mutable -> future<> {
                    auto in = s.input();
                    if (s.kind() == quic::stream_kind::unidirectional) {
                        co_await util::skip_entire_stream(in);
                        co_await in.close();
                        co_return;
                    }
                    auto out = s.output();
                    while (true) {
                        auto buf = co_await in.read();
                        if (buf.empty()) {
                            break;
                        }
                        co_await out.write(std::move(buf));
                        co_await out.flush();
                    }
                    co_await out.close();
                    co_await in.close();
                });
            }
        }(server_conn, echo_gate);
    }

    future<> stop() {
        co_await client.close();
        co_await server.stop();
        co_await std::move(echo_loop);
        co_await echo_gate.close();
    }
};

static future<std::unique_ptr<connected_pair>> make_connected_pair(
        quic::connection_config server_cfg = {}, quic::connection_config client_cfg = {}) {
    auto server_creds = co_await make_server_creds({"test/1"});
    auto client_creds = co_await make_client_creds();

    quic::listen_options lo;
    lo.config = server_cfg;
    lo.config.max_idle_timeout = std::chrono::seconds(10);
    auto server = quic::listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds, lo);
    auto addr = server.local_address();

    auto accept_fut = server.accept();
    auto copts = client_options({"test/1"});
    copts.config = client_cfg;
    copts.config.max_idle_timeout = std::chrono::seconds(10);
    auto client = co_await quic::connect(addr, client_creds, std::move(copts));
    auto server_conn = co_await std::move(accept_fut);

    auto pair = std::make_unique<connected_pair>();
    pair->server = std::move(server);
    pair->client = std::move(client);
    pair->server_conn = std::move(server_conn);
    co_return pair;
}

SEASTAR_TEST_CASE(test_quic_handshake_and_alpn) {
    auto pair = co_await make_connected_pair();

    BOOST_REQUIRE(pair->client.alpn().has_value());
    BOOST_REQUIRE_EQUAL(*pair->client.alpn(), "test/1");
    BOOST_REQUIRE(pair->server_conn.alpn().has_value());
    BOOST_REQUIRE_EQUAL(*pair->server_conn.alpn(), "test/1");

    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_bidi_stream_echo) {
    auto pair = co_await make_connected_pair();
    pair->start_echo();

    auto s = co_await pair->client.open_stream(quic::stream_kind::bidirectional);
    auto out = s.output();
    co_await out.write(sstring("hello quic"));
    co_await out.close();
    auto got = co_await read_all(s);
    BOOST_REQUIRE_EQUAL(got, "hello quic");

    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_alpn_mismatch_fails) {
    auto server_creds = co_await make_server_creds({"test/1"});
    auto client_creds = co_await make_client_creds();

    quic::listen_options lo;
    lo.config.max_idle_timeout = std::chrono::seconds(3);
    auto server = quic::listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds, lo);
    auto server_addr = server.local_address();

    (void)server.accept().then_wrapped([] (auto f) { f.ignore_ready_future(); });

    bool failed = false;
    try {
        auto client = co_await quic::connect(server_addr, client_creds, client_options({"other/2"}));
        (void)client;
    } catch (...) {
        failed = true;
    }
    BOOST_REQUIRE(failed);
    co_await server.stop();
}


SEASTAR_TEST_CASE(test_quic_large_transfer_with_backpressure) {
    // Keep the flow-control windows small so the transfer exercises
    // MAX_STREAM_DATA / MAX_DATA backpressure repeatedly.
    quic::connection_config cfg;
    cfg.initial_max_data = 128 << 10;
    cfg.initial_max_stream_data = 64 << 10;
    cfg.stream_send_buffer_size = 64 << 10;
    auto pair = co_await make_connected_pair(cfg, cfg);
    pair->start_echo();

    constexpr size_t total = 4 << 20; // 4 MiB
    auto s = co_await pair->client.open_stream(quic::stream_kind::bidirectional);

    // Reader: pull the echo back and count bytes.
    auto reader = [] (quic::stream& s) -> future<size_t> {
        auto in = s.input();
        size_t got = 0;
        while (true) {
            auto buf = co_await in.read();
            if (buf.empty()) {
                break;
            }
            got += buf.size();
        }
        co_await in.close();
        co_return got;
    }(s);

    auto out = s.output();
    temporary_buffer<char> chunk(16 << 10);
    std::memset(chunk.get_write(), 'x', chunk.size());
    for (size_t sent = 0; sent < total; sent += chunk.size()) {
        co_await out.write(chunk.clone());
    }
    co_await out.close();

    auto got = co_await std::move(reader);
    BOOST_REQUIRE_EQUAL(got, total);
    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_concurrent_streams) {
    auto pair = co_await make_connected_pair();
    pair->start_echo();
    constexpr int n = 64;

    co_await coroutine::parallel_for_each(std::views::iota(0, n), [&] (int i) -> future<> {
        auto s = co_await pair->client.open_stream(quic::stream_kind::bidirectional);
        auto payload = format("stream-{}", i);
        auto out = s.output();
        co_await out.write(payload);
        co_await out.close();
        auto got = co_await read_all(s);
        BOOST_REQUIRE_EQUAL(got, payload);
    });
    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_stream_credit_recycling) {
    // A closed peer-initiated stream must return stream credit to the peer
    // (MAX_STREAMS): with a limit of 4 concurrent streams, 12 sequential
    // fully-closed streams can only complete if credit is recycled.
    quic::connection_config server_cfg;
    server_cfg.max_streams_bidi = 4;
    auto pair = co_await make_connected_pair(server_cfg);
    pair->start_echo();

    auto run = [&] () -> future<> {
        for (int i = 0; i < 12; ++i) {
            auto s = co_await pair->client.open_stream(quic::stream_kind::bidirectional);
            auto payload = format("stream-{}", i);
            auto out = s.output();
            co_await out.write(payload);
            co_await out.close();
            auto got = co_await read_all(s);
            BOOST_REQUIRE_EQUAL(got, payload);
        }
    };
    // Bound the wait: without credit recycling the 5th open_stream would
    // block forever.
    co_await with_timeout(std::chrono::steady_clock::now() + std::chrono::seconds(30), run());
    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_uni_stream) {
    auto pair = co_await make_connected_pair();
    pair->start_echo();
    auto s = co_await pair->client.open_stream(quic::stream_kind::unidirectional);
    BOOST_REQUIRE(s.kind() == quic::stream_kind::unidirectional);
    auto out = s.output();
    co_await out.write(sstring("one way"));
    co_await out.close();
    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_stream_as_connected_socket) {
    // A bidirectional QUIC stream should behave as a connected_socket, so
    // protocol code written against connected_socket runs unchanged. Drive
    // both sides purely through the connected_socket API.
    auto pair = co_await make_connected_pair();

    // Server: accept a stream, adapt it to a connected_socket, echo a line.
    auto serve = pair->server_conn.accept_stream().then([] (quic::stream s) -> future<> {
        auto sock = std::move(s).to_connected_socket();
        auto in = sock.input();
        auto out = sock.output();
        auto line = co_await in.read();
        co_await out.write(std::move(line));
        co_await out.close();
        co_await in.close();
    });

    auto s = co_await pair->client.open_stream(quic::stream_kind::bidirectional);
    auto sock = std::move(s).to_connected_socket();
    BOOST_REQUIRE_EQUAL(sock.remote_address(), pair->client.remote_address());
    BOOST_REQUIRE(sock.get_nodelay());

    auto out = sock.output();
    co_await out.write(sstring("hello socket"));
    co_await out.close();
    auto in = sock.input();
    auto got = co_await util::read_entire_stream_contiguous(in);
    BOOST_REQUIRE_EQUAL(got, "hello socket");

    co_await std::move(serve);
    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_connected_socket_rejects_unidirectional) {
    auto pair = co_await make_connected_pair();
    auto s = co_await pair->client.open_stream(quic::stream_kind::unidirectional);
    BOOST_REQUIRE_THROW(std::move(s).to_connected_socket(), std::system_error);
    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_reset_stream_surfaces_to_peer) {
    auto pair = co_await make_connected_pair();

    // Server: accept a stream and try to read; expect an exception from
    // the peer's RESET_STREAM.
    auto server_read = pair->server_conn.accept_stream().then([] (quic::stream s) -> future<bool> {
        auto in = s.input();
        try {
            co_await util::skip_entire_stream(in);
            co_return false;
        } catch (...) {
            co_return true;
        }
    });

    auto s = co_await pair->client.open_stream(quic::stream_kind::bidirectional);
    auto out = s.output();
    co_await out.write(sstring("partial"));
    co_await out.flush();
    auto r = s.reset_write(quic::application_error_code{42});
    BOOST_REQUIRE(r.has_value());

    bool threw = co_await std::move(server_read);
    BOOST_REQUIRE(threw);
    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_connection_close_code) {
    auto pair = co_await make_connected_pair();
    auto closed = pair->server_conn.wait_closed();
    co_await pair->client.close(quic::application_error_code{7}, "bye");
    co_await std::move(closed);
    BOOST_REQUIRE(pair->server_conn.is_closed());
    co_await pair->server.stop();
    co_await std::move(pair->echo_loop);
    co_await pair->echo_gate.close();
}

SEASTAR_TEST_CASE(test_quic_idle_timeout) {
    quic::connection_config cfg;
    cfg.max_idle_timeout = std::chrono::milliseconds(300);
    auto server_creds = co_await make_server_creds({"test/1"});
    auto client_creds = co_await make_client_creds();
    quic::listen_options lo;
    lo.config = cfg;
    auto server = quic::listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds, lo);
    auto addr = server.local_address();
    auto accept_fut = server.accept();
    auto copts = client_options({"test/1"});
    copts.config = cfg;
    auto client = co_await quic::connect(addr, client_creds, std::move(copts));
    auto server_conn = co_await std::move(accept_fut);

    // No traffic: both ends should close on the idle timeout.
    co_await client.wait_closed();
    BOOST_REQUIRE(client.is_closed());
    co_await server.stop();
    (void)server_conn;
}

SEASTAR_TEST_CASE(test_quic_datagrams) {
    quic::connection_config cfg;
    cfg.enable_datagrams = true;
    auto pair = co_await make_connected_pair(cfg, cfg);

    co_await pair->client.send_datagram(temporary_buffer<char>("ping", 4));
    auto got = co_await pair->server_conn.receive_datagram();
    BOOST_REQUIRE_EQUAL(sstring(got.get(), got.size()), "ping");
    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_retry) {
    auto server_creds = co_await make_server_creds({"test/1"});
    auto client_creds = co_await make_client_creds();

    quic::listen_options lo;
    lo.config.max_idle_timeout = std::chrono::seconds(10);
    lo.require_retry = true; // force a Retry round trip for every new connection
    auto server = quic::listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds, lo);
    auto addr = server.local_address();

    auto accept_fut = server.accept();
    auto client = co_await quic::connect(addr, client_creds, client_options({"test/1"}));
    auto server_conn = co_await std::move(accept_fut);

    // Handshake succeeded despite the mandatory Retry.
    BOOST_REQUIRE(client.alpn().has_value());

    // Exchange a stream to confirm the connection is usable.
    auto echo = [] (quic::connection& conn, gate& g) -> future<> {
        auto s = co_await conn.accept_stream();
        (void)with_gate(g, [s = std::move(s)] () mutable -> future<> {
            auto in = s.input();
            auto out = s.output();
            auto buf = co_await in.read();
            co_await out.write(std::move(buf));
            co_await out.close();
            co_await in.close();
        });
    };
    gate g;
    auto echo_fut = echo(server_conn, g);
    auto s = co_await client.open_stream(quic::stream_kind::bidirectional);
    auto out = s.output();
    co_await out.write(sstring("retry-ok"));
    co_await out.close();
    BOOST_REQUIRE_EQUAL(co_await read_all(s), "retry-ok");

    co_await std::move(echo_fut);
    co_await client.close();
    co_await server.stop();
    co_await g.close();
}

SEASTAR_TEST_CASE(test_quic_client_migration) {
    auto pair = co_await make_connected_pair();
    pair->start_echo();

    // A first request/response confirms the handshake and lets the peer
    // supply spare connection IDs, both prerequisites for migration.
    {
        auto s0 = co_await pair->client.open_stream(quic::stream_kind::bidirectional);
        auto out0 = s0.output();
        co_await out0.write(sstring("warmup"));
        co_await out0.close();
        BOOST_REQUIRE_EQUAL(co_await read_all(s0), "warmup");
    }

    co_await pair->client.migrate(socket_address(ipv4_addr("127.0.0.1", 0)), /*immediate=*/false);

    // Exchange data over the migrated path.
    auto s = co_await pair->client.open_stream(quic::stream_kind::bidirectional);
    auto out = s.output();
    co_await out.write(sstring("after-migration"));
    co_await out.close();
    BOOST_REQUIRE_EQUAL(co_await read_all(s), "after-migration");
    co_await pair->stop();
}

SEASTAR_TEST_CASE(test_quic_session_resumption) {
    auto server_creds = co_await make_server_creds({"test/1"});
    auto client_creds = co_await make_client_creds();

    quic::listen_options lo;
    lo.config.max_idle_timeout = std::chrono::seconds(10);
    // Enable server-side session tickets.
    server_creds->set_session_resume_mode(tls::session_resume_mode::TLS13_SESSION_TICKET);
    auto server = quic::listen(socket_address(ipv4_addr("127.0.0.1", 0)), server_creds, lo);
    auto addr = server.local_address();

    // First connection: capture a session ticket.
    auto accept1 = server.accept();
    auto client1 = co_await quic::connect(addr, client_creds, client_options({"test/1"}));
    auto server1 = co_await std::move(accept1);
    auto ticket = co_await client1.wait_for_session_ticket();
    BOOST_REQUIRE(!ticket.data.empty());
    co_await client1.close();
    co_await server1.close();

    // Second connection: resume using the ticket.
    auto accept2 = server.accept();
    auto copts = client_options({"test/1"});
    copts.ticket = std::move(ticket);
    auto client2 = co_await quic::connect(addr, client_creds, std::move(copts));
    auto server2 = co_await std::move(accept2);
    BOOST_REQUIRE(client2.alpn().has_value());
    co_await client2.close();
    co_await server2.close();
    co_await server.stop();
}

SEASTAR_TEST_CASE(test_quic_transport_send_failure_tears_down_connection) {
    // A failure that escapes write_packets() (e.g. sendmsg() failing for a
    // reason quic_udp_channel's own GSO fallback doesn't retry) used to
    // bypass finish() entirely: run()'s only handler was start()'s
    // handle_exception, which just logs and returns — every queue/promise
    // the connection owns (wait_closed(), open streams' read/write, ...)
    // would then hang forever instead of failing. Simulate that failure by
    // shutting the client's channel down out from under a live connection,
    // and assert the connection still tears down instead of hanging.
    auto pair = co_await make_connected_pair();
    pair->start_echo();

    auto s = co_await pair->client.open_stream(quic::stream_kind::bidirectional);
    auto out = s.output();
    co_await out.write(sstring("hello"));
    co_await out.flush();

    quic::internal::api_access::impl(pair->client)->break_channel_for_testing();

    // Before the fix, wait_closed() would hang forever.
    co_await with_timeout(std::chrono::steady_clock::now() + std::chrono::seconds(10),
                          pair->client.wait_closed());

    // A stream operation on the torn-down connection must fail rather than
    // hang.
    bool stream_failed = false;
    try {
        co_await out.write(sstring("more"));
        co_await out.flush();
    } catch (...) {
        stream_failed = true;
    }
    BOOST_REQUIRE(stream_failed);

    // pair->stop() closes the (already-torn-down, so this is a idempotent
    // no-op on the client side) connection, stops the server — which
    // force-closes the server-side connection, unblocking echo_loop's
    // accept_stream() — and joins echo_loop/echo_gate.
    co_await pair->stop();
}


namespace {
// Per-shard state for the sharding test below, kept alive on each shard
// between the invoke_on_all setup and teardown steps.
struct shard_server_state {
    std::optional<quic::server> server;
    unsigned accepted = 0;
    seastar::gate accept_gate;
    future<> accept_loop = make_ready_future<>();
};
thread_local shard_server_state g_shard;
} // namespace

SEASTAR_TEST_CASE(test_quic_server_sharding_distributes_connections) {
    // The core sharding contract: bind one server per shard to the same
    // address (SO_REUSEPORT), and the kernel spreads incoming connections
    // across the shards' sockets by 4-tuple hash. Open many client
    // connections (each from a fresh ephemeral local port) and assert they
    // are handled by more than one shard.
    if (smp::count < 2) {
        co_return;
    }

    quic::listen_options lo;
    lo.config.max_idle_timeout = std::chrono::seconds(10);

    // Bind on the test shard first to obtain a concrete port, then bind
    // that same port on every other shard.
    auto server0_creds = co_await make_server_creds({"test/1"});
    g_shard.server = quic::listen(socket_address(ipv4_addr("127.0.0.1", 0)), server0_creds, lo);
    auto addr = g_shard.server->local_address();
    co_await smp::invoke_on_others(this_shard_id(), [addr, lo] () -> future<> {
        auto creds = co_await make_server_creds({"test/1"});
        g_shard.server = quic::listen(addr, creds, lo);
    });

    // Every shard drains its accept queue, counting connections and
    // keeping them alive until torn down.
    co_await smp::invoke_on_all([] () -> future<> {
        g_shard.accept_loop = [] () -> future<> {
            auto hold = g_shard.accept_gate.hold();
            std::vector<quic::connection> conns;
            while (true) {
                try {
                    conns.push_back(co_await g_shard.server->accept());
                } catch (...) {
                    break;
                }
                ++g_shard.accepted;
            }
            for (auto& c : conns) {
                co_await c.close();
            }
        }();
        co_return;
    });

    // Open a batch of client connections from the test shard. Each
    // quic::connect binds a fresh ephemeral local port, so the kernel's
    // reuseport hash distributes them across the server shards.
    constexpr int n = 24;
    auto client_creds = co_await make_client_creds();
    std::vector<quic::connection> clients;
    for (int i = 0; i < n; ++i) {
        clients.push_back(co_await quic::connect(addr, client_creds, client_options({"test/1"})));
    }

    // Give the accept loops a moment to observe every handshake.
    co_await seastar::sleep(std::chrono::milliseconds(200));

    for (auto& c : clients) {
        co_await c.close();
    }

    // Tear the servers down and count how the connections were spread.
    co_await smp::invoke_on_all([] () -> future<> {
        g_shard.server->abort_accept();
        co_await g_shard.server->stop();
        co_await std::move(g_shard.accept_loop);
        co_await g_shard.accept_gate.close();
    });
    auto per_shard = co_await map_reduce(
        smp::all_cpus(),
        [] (unsigned shard) { return smp::submit_to(shard, [] { return g_shard.accepted; }); },
        std::vector<unsigned>{},
        [] (std::vector<unsigned> v, unsigned c) { v.push_back(c); return v; });
    co_await smp::invoke_on_all([] { g_shard.server.reset(); });

    unsigned total = 0, shards_used = 0;
    for (auto c : per_shard) {
        total += c;
        if (c > 0) {
            ++shards_used;
        }
    }
    BOOST_TEST_MESSAGE(fmt::format("connections per shard: [{}] total={} shards_used={}",
                                   fmt::join(per_shard, ", "), total, shards_used));
    BOOST_REQUIRE_EQUAL(total, unsigned(n));
    // With 24 connections and >=2 shards the odds of a single shard taking
    // all of them are negligible (~2*0.5^24); reuseport must distribute.
    BOOST_REQUIRE_GE(shards_used, 2u);
}
