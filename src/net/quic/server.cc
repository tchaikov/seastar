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

#include "server.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/parallel_for_each.hh>

#include <ngtcp2/ngtcp2_crypto.h>

namespace seastar::net::quic::internal {

namespace {

// Per-server state keyed by bound port. The secret is generated on shard
// 0 and copied to every shard so CID shard-routing, retry tokens and
// stateless reset tokens agree across the whole server. Each shard also
// registers its live dispatcher here so datagrams misdelivered by the
// kernel's SO_REUSEPORT hashing can be forwarded to the owning shard.
struct per_port_state {
    server_secret secret;
    server_dispatcher* dispatcher = nullptr;
};
thread_local std::unordered_map<uint16_t, per_port_state> g_ports;

future<server_secret> fetch_secret(uint16_t port) {
    // submit_to() runs the function inline when already on shard 0.
    co_return co_await smp::submit_to(0, [port] {
        auto it = g_ports.find(port);
        if (it == g_ports.end()) {
            it = g_ports.emplace(port, per_port_state{server_secret::generate(), nullptr}).first;
        }
        return it->second.secret;
    });
}

constexpr ngtcp2_duration token_timeout = 3600ull * NGTCP2_SECONDS;

} // anonymous namespace

server_dispatcher::server_dispatcher(socket_address addr, shared_ptr<tls::server_credentials> creds,
                                     listen_options options)
    : _creds(std::move(creds))
    , _options(std::move(options))
    , _channel(addr, !_options.config.disable_gso)
    , _accept_q(_options.accept_queue_depth) {
}

server_dispatcher::~server_dispatcher() {
    auto it = g_ports.find(_channel.local_address().port());
    if (it != g_ports.end() && it->second.dispatcher == this) {
        it->second.dispatcher = nullptr;
    }
}

void server_dispatcher::start() {
    _receive_done = receive_loop().handle_exception([] (std::exception_ptr ep) {
        quic_log.warn("server dispatcher failed: {}", ep);
    });
}

void server_dispatcher::feed_forwarded(quic_udp_channel::rx_datagram dgram) {
    if (_stopping) {
        return;
    }
    // Hold the gate across this detached dispatch(), the same way
    // receive_loop() does for its own inline dispatch() calls: without it,
    // stop() (which awaits _gate.close()) does not wait for forwarded work
    // in flight, so a forwarded datagram that's still suspended (e.g. on
    // the channel send inside send_stateless_reset()) can resume after
    // this dispatcher has been torn down and use freed state.
    auto hold = _gate.try_hold();
    if (!hold) {
        return; // already tearing down; drop the forwarded datagram
    }
    (void)dispatch(std::move(dgram)).handle_exception([] (std::exception_ptr ep) {
        quic_log.debug("forwarded dispatch failed: {}", ep);
    }).finally([hold = std::move(*hold)] {});
}

future<net::quic::connection> server_dispatcher::accept() {
    return _accept_q.pop_eventually();
}

void server_dispatcher::abort_accept() {
    _accept_q.abort(std::make_exception_ptr(std::system_error(make_error_code(errc::aborted), "accept aborted")));
}

future<> server_dispatcher::stop(application_error_code code) noexcept {
    _stopping = true;
    _channel.close();
    // Fail any pending accept() so callers blocked there wake up.
    _accept_q.abort(std::make_exception_ptr(std::system_error(make_error_code(errc::aborted), "server stopped")));
    // Close every live connection.
    std::vector<lw_shared_ptr<quic_connection_impl>> conns;
    for (auto& [c, conn] : _connections) {
        if (std::ranges::find(conns, conn) == conns.end()) {
            conns.push_back(conn);
        }
    }
    co_await coroutine::parallel_for_each(conns, [code] (auto& conn) -> future<> {
        co_await conn->close(code, {});
    });
    co_await std::move(_receive_done);
    co_await _gate.close();
}

void server_dispatcher::connection_ready(lw_shared_ptr<quic_connection_impl> conn) noexcept {
    if (_stopping) {
        return;
    }
    if (!_accept_q.push(net::quic::connection(std::move(conn)))) {
        quic_log.warn("accept queue full; dropping connection");
    }
}

future<> server_dispatcher::receive_loop() {
    auto hold = _gate.hold();
    // Fetch the shared secret (generated once on shard 0) and register
    // this dispatcher so other shards can forward stray datagrams here.
    auto port = _channel.local_address().port();
    _secret = co_await fetch_secret(port);
    g_ports[port].dispatcher = this;

    while (!_channel.is_closed() && !_stopping) {
        quic_udp_channel::rx_datagram dgram;
        try {
            dgram = co_await _channel.receive();
        } catch (...) {
            break;
        }
        try {
            co_await dispatch(std::move(dgram));
        } catch (...) {
            quic_log.debug("dispatch failed: {}", std::current_exception());
        }
    }
}

future<> server_dispatcher::dispatch(quic_udp_channel::rx_datagram dgram) {
    // A GRO buffer may hold several datagrams; each is routed
    // independently (they can belong to different connections).
    size_t seg = dgram.gro_segment_size > 0 ? dgram.gro_segment_size : dgram.data.size();
    for (size_t off = 0; off < dgram.data.size(); off += seg) {
        auto len = std::min(seg, dgram.data.size() - off);
        auto piece = dgram.data.share(off, len);

        ngtcp2_version_cid vc;
        auto rv = ngtcp2_pkt_decode_version_cid(&vc, reinterpret_cast<const uint8_t*>(piece.get()), piece.size(),
                                                server_cid_len);
        if (rv == NGTCP2_ERR_VERSION_NEGOTIATION) {
            quic_udp_channel::rx_datagram sub{piece.clone(), dgram.src, dgram.dst, dgram.ecn, 0};
            co_await send_version_negotiation(vc, sub);
            continue;
        }
        if (rv != 0) {
            continue; // malformed; drop
        }

        auto dcid = cid::from(vc.dcid, vc.dcidlen);
        if (auto it = _connections.find(dcid); it != _connections.end()) {
            // Move the share (no copy): its refcount keeps the receive
            // buffer alive until the connection consumes it.
            it->second->feed_datagram(
                quic_udp_channel::rx_datagram{std::move(piece), dgram.src, dgram.dst, dgram.ecn, 0});
            continue;
        }

        // No local connection owns this DCID.
        auto shard = shard_of_cid(_secret, dcid.bytes());
        if (vc.dcidlen == server_cid_len && shard < smp::count && shard != this_shard_id()) {
            // Stray datagram for another shard's connection (e.g. after a
            // client rebind rehashed the 4-tuple): forward it once.
            auto data = std::vector<char>(piece.get(), piece.get() + piece.size());
            auto src = dgram.src;
            auto dst = dgram.dst;
            auto ecn = dgram.ecn;
            auto port = _channel.local_address().port();
            (void)smp::submit_to(shard, [port, data = std::move(data), src, dst, ecn] () mutable {
                auto it = g_ports.find(port);
                if (it == g_ports.end() || it->second.dispatcher == nullptr) {
                    return;
                }
                // Wrap the vector's storage instead of copying it again.
                auto* p = data.data();
                auto size = data.size();
                temporary_buffer<char> buf(p, size, make_object_deleter(std::move(data)));
                it->second.dispatcher->feed_forwarded(
                    quic_udp_channel::rx_datagram{std::move(buf), src, dst, ecn, 0});
            }).handle_exception([] (std::exception_ptr) {});
            continue;
        }

        quic_udp_channel::rx_datagram sub{piece.clone(), dgram.src, dgram.dst, dgram.ecn, 0};
        co_await handle_unmatched(std::move(sub), vc);
    }
}

future<> server_dispatcher::handle_unmatched(quic_udp_channel::rx_datagram dgram, const ngtcp2_version_cid& vc) {
    ngtcp2_pkt_hd hd;
    auto accept_rv = ngtcp2_accept(&hd, reinterpret_cast<const uint8_t*>(dgram.data.get()), dgram.data.size());
    if (accept_rv != 0) {
        // Not a valid new-connection Initial; short-header packet with an
        // unknown CID gets a stateless reset.
        co_await send_stateless_reset(vc, dgram);
        co_return;
    }

    ngtcp2_cid odcid = hd.dcid;
    const ngtcp2_cid* retry_scid = nullptr;
    ngtcp2_cid retry_scid_storage;
    std::span<const uint8_t> token;
    ngtcp2_token_type token_type = NGTCP2_TOKEN_TYPE_UNKNOWN;

    if (hd.tokenlen > 0) {
        // Try a Retry token first, then a NEW_TOKEN regular token.
        ngtcp2_cid decoded_odcid;
        auto& remote = dgram.src;
        if (ngtcp2_crypto_verify_retry_token2(&decoded_odcid, hd.token, hd.tokenlen,
                                              _secret.data.data(), _secret.data.size(), hd.version,
                                              &remote.u.sa, remote.addr_length, &hd.dcid,
                                              token_timeout, quic_now()) == 0) {
            odcid = decoded_odcid;
            retry_scid_storage = hd.dcid;
            retry_scid = &retry_scid_storage;
            token = {hd.token, hd.tokenlen};
            token_type = NGTCP2_TOKEN_TYPE_RETRY;
        } else if (ngtcp2_crypto_verify_regular_token(hd.token, hd.tokenlen,
                                                      _secret.data.data(), _secret.data.size(),
                                                      &remote.u.sa, remote.addr_length,
                                                      token_timeout, quic_now()) == 0) {
            token = {hd.token, hd.tokenlen};
            token_type = NGTCP2_TOKEN_TYPE_NEW_TOKEN;
        } else if (_options.require_retry) {
            co_await send_retry(hd, dgram);
            co_return;
        }
        // An invalid token when retry is not required is ignored: proceed
        // to a normal handshake (the client will be validated by the
        // handshake itself).
    } else if (_options.require_retry) {
        co_await send_retry(hd, dgram);
        co_return;
    }

    try {
        // make_server queues the triggering Initial internally so its
        // fiber installs Initial keys before writing.
        co_await quic_connection_impl::make_server(shared_from_this(), _creds, _options.config, hd,
                                                   odcid, retry_scid, token, token_type, dgram);
    } catch (...) {
        quic_log.debug("failed to create server connection: {}", std::current_exception());
    }
}

// Sends a dispatcher-generated packet (version negotiation, Retry,
// stateless reset) back to the datagram's origin.
future<> server_dispatcher::send_reply(temporary_buffer<char> buf, size_t len,
                                       const quic_udp_channel::rx_datagram& dgram) {
    buf.trim(len);
    quic_udp_channel::tx_batch batch;
    batch.data = std::move(buf);
    batch.dst = dgram.src;
    batch.src = dgram.dst;
    co_await _channel.send(std::move(batch));
}

future<> server_dispatcher::send_version_negotiation(const ngtcp2_version_cid& vc,
                                                     const quic_udp_channel::rx_datagram& dgram) {
    std::array<uint32_t, 2> versions{NGTCP2_PROTO_VER_V1, NGTCP2_PROTO_VER_V2};
    temporary_buffer<char> buf(1200);
    // A reserved version with the 0x?a?a?a?a "greasing" pattern (RFC 9000
    // §6.3) is offered first to exercise clients' version handling.
    uint32_t reserved = 0x0a0a0a0au;
    auto nwrite = ngtcp2_pkt_write_version_negotiation(
        reinterpret_cast<uint8_t*>(buf.get_write()), buf.size(), reserved,
        vc.scid, vc.scidlen, vc.dcid, vc.dcidlen, versions.data(), versions.size());
    if (nwrite < 0) {
        co_return;
    }
    co_await send_reply(std::move(buf), nwrite, dgram);
}

future<> server_dispatcher::send_retry(const ngtcp2_pkt_hd& hd, const quic_udp_channel::rx_datagram& dgram) {
    // Choose a fresh SCID for the Retry; the client echoes it back and we
    // recover the original DCID from the token.
    auto new_scid = make_server_cid(_secret, this_shard_id());
    auto nscid = new_scid.to_ngtcp2();

    std::array<uint8_t, NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2> token;
    auto& remote = dgram.src;
    auto tokenlen = ngtcp2_crypto_generate_retry_token2(
        token.data(), _secret.data.data(), _secret.data.size(), hd.version,
        &remote.u.sa, remote.addr_length, &nscid, &hd.dcid, quic_now());
    if (tokenlen < 0) {
        co_return;
    }

    temporary_buffer<char> buf(1200);
    auto nwrite = ngtcp2_crypto_write_retry(reinterpret_cast<uint8_t*>(buf.get_write()), buf.size(),
                                            hd.version, &hd.scid, &nscid, &hd.dcid, token.data(), tokenlen);
    if (nwrite < 0) {
        co_return;
    }
    co_await send_reply(std::move(buf), nwrite, dgram);
}

future<> server_dispatcher::send_stateless_reset(const ngtcp2_version_cid& vc,
                                                 const quic_udp_channel::rx_datagram& dgram) {
    // Rate-limit and only respond to packets large enough to plausibly
    // belong to a real connection (RFC 9000 §10.3).
    auto tick = lowres_clock::now();
    if (tick - _reset_budget_reset > std::chrono::seconds(1)) {
        _reset_budget = 100;
        _reset_budget_reset = tick;
    }
    if (_reset_budget == 0 || dgram.data.size() < NGTCP2_MIN_STATELESS_RESET_RANDLEN + 22) {
        co_return;
    }
    --_reset_budget;

    auto dcid = cid::from(vc.dcid, vc.dcidlen);
    auto ndcid = dcid.to_ngtcp2();
    ngtcp2_stateless_reset_token token;
    if (ngtcp2_crypto_generate_stateless_reset_token(token.data, _secret.data.data(), _secret.data.size(),
                                                     &ndcid) != 0) {
        co_return;
    }

    temporary_buffer<char> buf(dgram.data.size() - 1);
    std::array<uint8_t, NGTCP2_MIN_STATELESS_RESET_RANDLEN + 32> rand_data;
    random_bytes(rand_data);
    auto nwrite = ngtcp2_pkt_write_stateless_reset(reinterpret_cast<uint8_t*>(buf.get_write()), buf.size(),
                                                   token.data, rand_data.data(), rand_data.size());
    if (nwrite < 0) {
        co_return;
    }
    co_await send_reply(std::move(buf), nwrite, dgram);
}

} // namespace seastar::net::quic::internal
