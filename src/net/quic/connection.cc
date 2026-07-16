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

#include "connection.hh"
#include "server.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/log.hh>

#include <ngtcp2/ngtcp2_crypto.h>

#include <fmt/format.h>

namespace seastar::net::quic {

logger quic_log("seastar-quic");

namespace internal {

namespace {

socket_address to_socket_address(const ngtcp2_addr& addr) {
    socket_address sa{};
    std::memcpy(&sa.u.sa, addr.addr, addr.addrlen);
    sa.addr_length = addr.addrlen;
    return sa;
}

ngtcp2_addr to_ngtcp2_addr(socket_address& sa) {
    return ngtcp2_addr{&sa.u.sa, sa.addr_length};
}

// Builds an ngtcp2_path referencing the given addresses; they must stay
// alive for the duration of the call the path is passed to.
ngtcp2_path make_path(socket_address& local, socket_address& remote) {
    return ngtcp2_path{to_ngtcp2_addr(local), to_ngtcp2_addr(remote), nullptr};
}

std::chrono::nanoseconds to_duration(ngtcp2_duration d) {
    return std::chrono::nanoseconds(d);
}

} // anonymous namespace

quic_connection_impl::quic_connection_impl() = default;

quic_connection_impl::~quic_connection_impl() {
    if (_conn) {
        ngtcp2_conn_del(_conn);
        _conn = nullptr;
    }
}

ngtcp2_conn* quic_connection_impl::get_conn_cb(ngtcp2_crypto_conn_ref* ref) noexcept {
    return static_cast<quic_connection_impl*>(ref->user_data)->_conn;
}

ngtcp2_callbacks quic_connection_impl::make_callbacks(bool server) noexcept {
    ngtcp2_callbacks cb{};
    // Handshake machinery from the ngtcp2 crypto helper.
    if (server) {
        cb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    } else {
        cb.client_initial = ngtcp2_crypto_client_initial_cb;
        cb.recv_retry = ngtcp2_crypto_recv_retry_cb;
    }
    cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    cb.encrypt = ngtcp2_crypto_encrypt_cb;
    cb.decrypt = ngtcp2_crypto_decrypt_cb;
    cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
    cb.update_key = ngtcp2_crypto_update_key_cb;
    cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    cb.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
    cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;

    cb.handshake_completed = on_handshake_completed;
    cb.handshake_confirmed = on_handshake_confirmed;
    cb.recv_stream_data = on_recv_stream_data;
    cb.acked_stream_data_offset = on_acked_stream_data_offset;
    cb.stream_open = on_stream_open;
    cb.stream_close = on_stream_close;
    cb.stream_reset = on_stream_reset;
    cb.stream_stop_sending = on_stream_stop_sending;
    cb.extend_max_local_streams_bidi = on_extend_max_streams_bidi;
    cb.extend_max_local_streams_uni = on_extend_max_streams_uni;
    cb.extend_max_stream_data = on_extend_max_stream_data;
    cb.rand = on_rand;
    cb.get_new_connection_id2 = on_get_new_connection_id;
    cb.remove_connection_id = on_remove_connection_id;
    cb.path_validation = on_path_validation;
    cb.recv_datagram = on_recv_datagram;
    cb.recv_new_token = on_recv_new_token;
    cb.recv_stateless_reset = on_recv_stateless_reset;
    return cb;
}

void quic_connection_impl::setup_settings_and_params(ngtcp2_settings& settings, ngtcp2_transport_params& params) const {
    ngtcp2_settings_default(&settings);
    settings.initial_ts = now();
    // Let ngtcp2 grow the flow-control windows up to these ceilings based on
    // the bandwidth-delay product, so a bulk transfer is not pinned to the
    // small initial windows.
    settings.max_stream_window = _config.max_stream_data_window;
    settings.max_window = _config.max_data_window;

    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi = _config.max_streams_bidi;
    params.initial_max_streams_uni = _config.max_streams_uni;
    params.initial_max_data = _config.initial_max_data;
    params.initial_max_stream_data_bidi_local = _config.initial_max_stream_data;
    params.initial_max_stream_data_bidi_remote = _config.initial_max_stream_data;
    params.initial_max_stream_data_uni = _config.initial_max_stream_data;
    params.max_ack_delay = std::chrono::duration_cast<std::chrono::nanoseconds>(_config.max_ack_delay).count();
    params.max_idle_timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(_config.max_idle_timeout).count();
    params.active_connection_id_limit = _config.active_connection_id_limit;
    if (_config.enable_datagrams) {
        params.max_datagram_frame_size = 65535;
    }
}

future<lw_shared_ptr<quic_connection_impl>> quic_connection_impl::make_client(
        socket_address remote, shared_ptr<tls::certificate_credentials> creds, connect_options options) {
    if (options.alpn_protocols.empty()) {
        throw std::invalid_argument("QUIC requires at least one ALPN protocol (connect_options::alpn_protocols)");
    }

    auto conn = make_lw_shared<quic_connection_impl>();
    conn->_config = options.config;
    conn->_is_server = false;

    socket_address local = remote.family() == AF_INET ? socket_address(ipv4_addr()) : socket_address(ipv6_addr());
    conn->_own_channel = std::make_unique<quic_udp_channel>(local, !options.config.disable_gso);
    conn->_channel = conn->_own_channel.get();
    conn->_local_addr = conn->_channel->local_address();
    conn->_remote_addr = remote;

    conn->_holder.conn_ref = ngtcp2_crypto_conn_ref{get_conn_cb, conn.get()};
    client_tls_options tls_options{
        .server_name = std::move(options.server_name),
        .alpn_protocols = std::move(options.alpn_protocols),
        .ticket = std::move(options.ticket),
    };
    conn->_tls = co_await make_client_tls_session(std::move(creds), conn->_holder, std::move(tls_options));
    conn->_tls->set_ticket_callback([c = conn.get()] (session_ticket ticket) {
        if (!c->_ticket_resolved) {
            c->_ticket_resolved = true;
            c->_session_ticket.set_value(std::move(ticket));
        }
    });

    ngtcp2_cid dcid, scid;
    dcid.datalen = 16;
    random_bytes({dcid.data, dcid.datalen});
    scid.datalen = 16;
    random_bytes({scid.data, scid.datalen});

    ngtcp2_settings settings;
    ngtcp2_transport_params params;
    conn->setup_settings_and_params(settings, params);
    if (options.token && !options.token->data.empty()) {
        settings.token = options.token->data.data();
        settings.tokenlen = options.token->data.size();
        settings.token_type = NGTCP2_TOKEN_TYPE_NEW_TOKEN;
    }

    auto path = make_path(conn->_local_addr, conn->_remote_addr);
    auto callbacks = make_callbacks(false);
    if (auto rv = ngtcp2_conn_client_new(&conn->_conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1,
                                         &callbacks, &settings, &params, nullptr, conn.get());
        rv != 0) {
        throw std::system_error(make_error_code(errc::internal), ngtcp2_strerror(rv));
    }
    ngtcp2_conn_set_tls_native_handle(conn->_conn, conn->_tls->native_handle());
    if (conn->_config.keep_alive_interval.count() > 0) {
        ngtcp2_conn_set_keep_alive_timeout(conn->_conn,
            std::chrono::duration_cast<std::chrono::nanoseconds>(conn->_config.keep_alive_interval).count());
    }

    conn->start();
    co_return conn;
}

future<lw_shared_ptr<quic_connection_impl>> quic_connection_impl::make_server(
        lw_shared_ptr<server_dispatcher> dispatcher, shared_ptr<tls::server_credentials> creds,
        const connection_config& config, const ngtcp2_pkt_hd& hd,
        const ngtcp2_cid& odcid, const ngtcp2_cid* retry_scid,
        std::span<const uint8_t> token, ngtcp2_token_type token_type,
        const quic_udp_channel::rx_datagram& first_dgram) {
    auto conn = make_lw_shared<quic_connection_impl>();
    conn->_config = config;
    conn->_is_server = true;
    conn->_dispatcher = dispatcher;
    conn->_channel = &dispatcher->channel();
    conn->_local_addr = first_dgram.dst;
    conn->_remote_addr = first_dgram.src;

    conn->_holder.conn_ref = ngtcp2_crypto_conn_ref{get_conn_cb, conn.get()};
    conn->_tls = co_await make_server_tls_session(std::move(creds), conn->_holder);

    ngtcp2_settings settings;
    ngtcp2_transport_params params;
    conn->setup_settings_and_params(settings, params);
    if (!token.empty()) {
        settings.token = token.data();
        settings.tokenlen = token.size();
        settings.token_type = token_type;
    }
    params.original_dcid = odcid;
    params.original_dcid_present = 1;
    if (retry_scid != nullptr) {
        params.retry_scid = *retry_scid;
        params.retry_scid_present = 1;
    }

    // The server's initial SCID embeds the owning shard for datagram
    // routing.
    auto scid_c = make_server_cid(dispatcher->secret(), this_shard_id());
    auto scid = scid_c.to_ngtcp2();

    auto local = first_dgram.dst;
    auto remote = first_dgram.src;
    auto path = make_path(local, remote);
    auto callbacks = make_callbacks(true);
    if (auto rv = ngtcp2_conn_server_new(&conn->_conn, &hd.scid, &scid, &path, hd.version,
                                         &callbacks, &settings, &params, nullptr, conn.get());
        rv != 0) {
        throw std::system_error(make_error_code(errc::internal), ngtcp2_strerror(rv));
    }
    ngtcp2_conn_set_tls_native_handle(conn->_conn, conn->_tls->native_handle());
    if (config.keep_alive_interval.count() > 0) {
        ngtcp2_conn_set_keep_alive_timeout(conn->_conn,
            std::chrono::duration_cast<std::chrono::nanoseconds>(config.keep_alive_interval).count());
    }

    // Route the client-chosen destination CID and our SCID to this
    // connection.
    dispatcher->associate_cid(cid::from(odcid), conn);
    conn->_local_cids.push_back(cid::from(odcid));
    dispatcher->associate_cid(scid_c, conn);
    conn->_local_cids.push_back(scid_c);

    // Queue the Initial that opened this connection so the fiber processes
    // it (installing Initial keys) before its first write attempt.
    conn->_rx.push_back(quic_udp_channel::rx_datagram{
        first_dgram.data.clone(), first_dgram.src, first_dgram.dst, first_dgram.ecn, 0});
    conn->start();
    co_return conn;
}

void quic_connection_impl::start() {
    _expiry_timer.set_callback([this] {
        _wake.signal();
    });
    // The connection fiber; holds a self-reference so the connection
    // survives until it finishes even if all handles are dropped.
    (void)run().handle_exception([] (std::exception_ptr ep) {
        quic_log.warn("connection fiber failed: {}", ep);
    }).finally([self = shared_from_this()] {});
    if (!_is_server) {
        (void)client_receive_loop(_channel).handle_exception([] (std::exception_ptr) {
        }).finally([self = shared_from_this()] {});
    }
}

future<> quic_connection_impl::client_receive_loop(quic_udp_channel* channel) {
    auto hold = _gate.hold();
    while (!channel->is_closed() && _state != state::closed) {
        quic_udp_channel::rx_datagram dgram;
        try {
            dgram = co_await channel->receive();
        } catch (...) {
            break;
        }
        feed_datagram(std::move(dgram));
    }
}

void quic_connection_impl::feed_datagram(quic_udp_channel::rx_datagram dgram, bool forwarded) {
    if (_state == state::closed) {
        return;
    }
    if (forwarded) {
        ++_forwarded_datagrams;
    }
    _bytes_received += dgram.data.size();
    _rx.push_back(std::move(dgram));
    _wake.signal();
}

void quic_connection_impl::process_rx() {
    while (!_rx.empty() && _state != state::closed && _state != state::draining) {
        auto dgram = std::move(_rx.front());
        _rx.pop_front();

        // For clients, keep the local path address stable (the bound
        // wildcard address) so ngtcp2 does not see phantom path changes;
        // servers use the pktinfo destination to reply from the right
        // address.
        auto local = _is_server ? dgram.dst : _local_addr;
        auto path = make_path(local, dgram.src);
        ngtcp2_pkt_info pi{};
        pi.ecn = dgram.ecn;

        // A GRO buffer holds several consecutive UDP datagrams.
        size_t seg = dgram.gro_segment_size > 0 ? dgram.gro_segment_size : dgram.data.size();
        for (size_t off = 0; off < dgram.data.size(); off += seg) {
            auto len = std::min(seg, dgram.data.size() - off);
            auto rv = ngtcp2_conn_read_pkt(_conn, &path, &pi,
                                           reinterpret_cast<const uint8_t*>(dgram.data.get()) + off, len, now());
            if (rv == 0) {
                continue;
            }
            switch (rv) {
            case NGTCP2_ERR_DRAINING:
                enter_draining();
                return;
            case NGTCP2_ERR_DROP_CONN:
            case NGTCP2_ERR_RETRY:
                // Drop the connection without a CONNECTION_CLOSE (RETRY is
                // only valid in the accept path). finish() runs in the fiber.
                _error = std::make_exception_ptr(
                    std::system_error(make_error_code(errc::dropped_connection)));
                _state = state::closed;
                return;
            case NGTCP2_ERR_CRYPTO:
            default:
                handle_ngtcp2_failure(rv);
                return;
            }
        }
    }
}

void quic_connection_impl::enter_draining() {
    _state = state::draining;
    _close_deadline = std::chrono::steady_clock::now() + 3 * to_duration(ngtcp2_conn_get_pto2(_conn));
    if (!_error) {
        auto* ccerr = ngtcp2_conn_get_ccerr(_conn);
        _error = std::make_exception_ptr(connection_error(
            ccerr->error_code, ccerr->type == NGTCP2_CCERR_TYPE_APPLICATION,
            sstring(reinterpret_cast<const char*>(ccerr->reason), ccerr->reasonlen)));
    }
}

void quic_connection_impl::handle_ngtcp2_failure(int rv) {
    if (is_terminating()) {
        return;
    }
    quic_log.debug("connection failure: {}", ngtcp2_strerror(rv));
    _last_ccerr = *ngtcp2_conn_get_ccerr(_conn);
    if (_last_ccerr.error_code == 0 && _last_ccerr.type == NGTCP2_CCERR_TYPE_TRANSPORT) {
        ngtcp2_ccerr_set_liberr(&_last_ccerr, rv, nullptr, 0);
    }
    if (!_error) {
        if (rv == NGTCP2_ERR_CRYPTO) {
            _error = std::make_exception_ptr(std::system_error(make_error_code(errc::crypto_error),
                                                               ngtcp2_strerror(rv)));
        } else {
            _error = std::make_exception_ptr(std::system_error(make_error_code(errc::internal),
                                                               ngtcp2_strerror(rv)));
        }
    }
    _close_requested = true;
}

// Produces exactly one QUIC packet into [dest, dest+destlen), draining
// pending datagrams and stream data via WRITE_MORE coalescing. Returns the
// packet length, 0 when there is nothing (more) to send, or a negative
// ngtcp2 error. Follows the ngtcp2 examples' write_pkt structure.
ngtcp2_ssize quic_connection_impl::write_one_packet(ngtcp2_path_storage& ps, ngtcp2_pkt_info& pi,
                                                    uint8_t* dest, size_t destlen) {
    for (;;) {
        // Unreliable DATAGRAM frames first; each shares a packet with
        // whatever stream data fits.
        if (!_tx_datagrams.empty()) {
            auto& [dgram, pr] = _tx_datagrams.front();
            ngtcp2_vec vec{reinterpret_cast<uint8_t*>(dgram.get_write()), dgram.size()};
            int accepted = 0;
            auto nwrite = ngtcp2_conn_writev_datagram(_conn, &ps.path, &pi, dest, destlen,
                                                      &accepted, NGTCP2_WRITE_DATAGRAM_FLAG_MORE,
                                                      0, &vec, 1, now());
            if (nwrite == NGTCP2_ERR_INVALID_STATE || nwrite == NGTCP2_ERR_INVALID_ARGUMENT) {
                pr.set_exception(std::make_exception_ptr(std::system_error(
                    make_error_code(nwrite == NGTCP2_ERR_INVALID_STATE ? errc::unsupported : errc::invalid_argument),
                    "DATAGRAM send failed")));
                _tx_datagrams.pop_front();
                continue;
            }
            if (accepted) {
                pr.set_value();
                _tx_datagrams.pop_front();
            }
            if (nwrite == NGTCP2_ERR_WRITE_MORE) {
                continue;
            }
            if (nwrite != 0 || accepted) {
                return nwrite;
            }
            // nwrite == 0 and not accepted (blocked): fall through to
            // stream data.
        }

        // Pick the next stream with unsent data that is not flow-control
        // blocked.
        lw_shared_ptr<quic_stream_impl> stream;
        while (!_send_ready.empty()) {
            auto candidate = _send_ready.front();
            if (!candidate->wants_send()) {
                pop_send_ready();
                continue;
            }
            if (candidate->blocked()) {
                pop_send_ready();
                continue; // revisited when its window opens
            }
            stream = candidate;
            break;
        }

        std::array<ngtcp2_vec, 16> vec;
        size_t veccnt = 0;
        int64_t sid = -1;
        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        size_t offered = 0;
        bool offer_fin = false;
        if (stream) {
            sid = stream->id();
            veccnt = stream->pending_vectors(vec);
            for (size_t i = 0; i < veccnt; ++i) {
                offered += vec[i].len;
            }
            offer_fin = stream->fin_pending();
            if (offer_fin) {
                flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
            }
        }

        ngtcp2_ssize ndatalen = -1;
        auto nwrite = ngtcp2_conn_writev_stream(_conn, &ps.path, &pi, dest, destlen,
                                                &ndatalen, flags, sid, vec.data(), veccnt, now());
        if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
            stream->set_blocked(true);
            pop_send_ready();
            continue;
        }
        if (nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
            pop_send_ready();
            continue;
        }
        if (ndatalen >= 0 && stream) {
            auto n = static_cast<size_t>(ndatalen);
            stream->advance_submitted(n, offer_fin && n == offered);
            if (!stream->wants_send()) {
                pop_send_ready();
            }
        }
        if (nwrite == NGTCP2_ERR_WRITE_MORE) {
            continue;
        }
        return nwrite;
    }
}

future<> quic_connection_impl::write_packets() {
    while (_state == state::handshaking || _state == state::established) {
        const size_t max_udp = ngtcp2_conn_get_max_tx_udp_payload_size(_conn);
        const size_t gso_segments = _config.disable_gso ? 1 : _channel->max_gso_segments();
        const size_t cap = max_udp * gso_segments;

        temporary_buffer<char> buf(cap);
        auto* base = reinterpret_cast<uint8_t*>(buf.get_write());
        size_t written = 0;
        size_t seg_size = 0;
        bool batch_done = false;
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi{};

        while (written + max_udp <= cap) {
            auto nwrite = write_one_packet(ps, pi, base + written, max_udp);
            if (nwrite < 0) {
                handle_ngtcp2_failure(static_cast<int>(nwrite));
                co_return;
            }
            if (nwrite == 0) {
                batch_done = true; // nothing (more) to send this round
                break;
            }
            written += nwrite;
            if (seg_size == 0) {
                seg_size = nwrite;
            } else if (static_cast<size_t>(nwrite) < seg_size) {
                break; // a short segment must end the GSO batch
            }
        }

        if (written == 0) {
            ngtcp2_conn_update_pkt_tx_time(_conn, now());
            co_return;
        }

        buf.trim(written);
        quic_udp_channel::tx_batch batch;
        batch.data = std::move(buf);
        batch.dst = to_socket_address(ps.path.remote);
        batch.segment_size = seg_size;
        batch.ecn = static_cast<uint8_t>(pi.ecn);
        if (_is_server) {
            batch.src = to_socket_address(ps.path.local);
        }
        _bytes_sent += written;
        // Snapshot the channel: migrate() may retire it (swapping _channel
        // to a new socket and closing this one) while this send is
        // suspended. A failure on a channel that has since been retired is
        // just a lost packet — QUIC recovers via retransmission — not a
        // transport error worth killing the connection over. A failure on
        // the *current* channel (channel == _channel) is real and
        // propagates to tear the connection down via run()'s safety net.
        auto* channel = _channel;
        try {
            co_await channel->send(std::move(batch));
        } catch (...) {
            if (channel == _channel) {
                throw;
            }
        }
        ngtcp2_conn_update_pkt_tx_time(_conn, now());

        if (batch_done) {
            co_return;
        }
    }
}

void quic_connection_impl::rearm_timer() {
    if (_state == state::closed) {
        return;
    }
    auto expiry = ngtcp2_conn_get_expiry2(_conn);
    if (expiry == UINT64_MAX) {
        _expiry_timer.cancel();
        return;
    }
    auto deadline = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(expiry));
    _expiry_timer.rearm(deadline);
}

future<> quic_connection_impl::run() {
    // A failure that escapes run_loop() (e.g. the UDP channel's sendmsg()
    // failing for a reason write_packets() doesn't itself retry/handle,
    // such as ENETUNREACH or a transient ENOBUFS) must still tear the
    // connection down. Without this, the exception would propagate
    // straight to start()'s handle_exception (which only logs), leaving
    // finish() never called: every queue/promise this connection owns
    // (accept_stream, streams' receive()/write(), wait_closed(), ...)
    // would then hang forever instead of failing, since nothing else ever
    // resolves them once this fiber is gone. co_await is not allowed
    // inside a catch handler, so the exception is captured here and acted
    // on afterwards.
    std::exception_ptr failure;
    try {
        co_await run_loop();
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        if (!_error) {
            _error = failure;
        }
        co_await finish();
        std::rethrow_exception(failure);
    }
}

future<> quic_connection_impl::run_loop() {
    while (_state == state::handshaking || _state == state::established) {
        process_rx();

        if (_state == state::handshaking || _state == state::established) {
            // Handle expired timers before producing packets.
            if (auto expiry = ngtcp2_conn_get_expiry2(_conn); expiry <= now()) {
                if (auto rv = ngtcp2_conn_handle_expiry(_conn, now()); rv != 0) {
                    if (rv == NGTCP2_ERR_IDLE_CLOSE) {
                        _error = std::make_exception_ptr(std::system_error(make_error_code(errc::idle_close)));
                        co_return co_await finish();
                    }
                    handle_ngtcp2_failure(rv);
                }
            }
        }

        if (_close_requested && (_state == state::handshaking || _state == state::established)) {
            co_await send_connection_close();
            break;
        }
        if (_state == state::closed) {
            co_return co_await finish();
        }
        if (_state == state::draining) {
            break;
        }

        // Events that arrive while write_packets() is suspended set
        // _tx_pending again and re-run the loop; clearing it first makes
        // that window race-free.
        _tx_pending = false;
        co_await write_packets();

        if (_close_requested && (_state == state::handshaking || _state == state::established)) {
            co_await send_connection_close();
            break;
        }
        if (_state == state::draining || _state == state::closed) {
            break;
        }

        rearm_timer();
        co_await _wake.when([this] {
            return !_rx.empty() || _close_requested || !_tx_datagrams.empty()
                || _tx_pending || _expiry_due()
                || (_state != state::handshaking && _state != state::established);
        });
    }

    // Closing/draining period: sit out ~3 PTO so late packets do not
    // spawn a new connection on the peer.
    if (_state == state::closing || _state == state::draining) {
        auto now_tp = std::chrono::steady_clock::now();
        if (now_tp < _close_deadline) {
            co_await seastar::sleep<std::chrono::steady_clock>(_close_deadline - now_tp);
        }
    }
    co_await finish();
}

bool quic_connection_impl::_expiry_due() const noexcept {
    return ngtcp2_conn_get_expiry2(_conn) <= now();
}

future<> quic_connection_impl::send_connection_close() {
    if (is_terminating()) {
        co_return;
    }
    temporary_buffer<char> buf(1500);
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi{};
    auto nwrite = ngtcp2_conn_write_connection_close(_conn, &ps.path, &pi,
                                                     reinterpret_cast<uint8_t*>(buf.get_write()), buf.size(),
                                                     &_last_ccerr, now());
    _state = state::closing;
    _close_deadline = std::chrono::steady_clock::now() + 3 * to_duration(ngtcp2_conn_get_pto2(_conn));
    if (nwrite > 0) {
        buf.trim(nwrite);
        quic_udp_channel::tx_batch batch;
        batch.data = std::move(buf);
        batch.dst = to_socket_address(ps.path.remote);
        batch.segment_size = static_cast<size_t>(nwrite);
        if (_is_server) {
            batch.src = to_socket_address(ps.path.local);
        }
        try {
            co_await _channel->send(std::move(batch));
        } catch (...) {
            // Losing the CONNECTION_CLOSE only delays the peer's cleanup.
        }
    }
}

future<> quic_connection_impl::finish() {
    if (_finished) {
        co_return;
    }
    _finished = true;
    _state = state::closed;
    _expiry_timer.cancel();

    if (!_error) {
        _error = std::make_exception_ptr(std::system_error(make_error_code(errc::closing)));
    }

    if (!_handshake_resolved) {
        _handshake_resolved = true;
        _handshake_done.set_exception(_error);
    }
    for (auto& [id, stream] : _streams) {
        stream->on_connection_error(_error);
    }
    // Break the connection<->stream ownership cycle (streams hold a
    // lw_shared_ptr back to the connection).
    _streams.clear();
    for (auto& s : _send_ready) {
        s->set_in_send_ready(false);
    }
    _send_ready.clear();
    _accept_q.abort(_error);
    _rx_datagrams.abort(_error);
    for (auto& [buf, pr] : _tx_datagrams) {
        pr.set_exception(_error);
    }
    _tx_datagrams.clear();
    if (_path_validated) {
        _path_validated->set_exception(_error);
        _path_validated.reset();
    }
    if (_ticket_waited && !_ticket_resolved) {
        _ticket_resolved = true;
        _session_ticket.set_exception(_error);
    }
    if (_dispatcher) {
        for (const auto& c : _local_cids) {
            _dispatcher->dissociate_cid(c);
        }
        _local_cids.clear();
    }
    if (_own_channel) {
        _own_channel->close();
    }
    for (auto& ch : _retired_channels) {
        ch->close();
    }
    _closed.set_value();
    // Wait for the receive loops (which hold the gate) to finish before
    // returning; the channels they read from stay alive until this object
    // is destroyed.
    co_await _gate.close();
}

// Public API.

future<stream> quic_connection_impl::open_stream(stream_kind kind) {
    while (true) {
        if (is_terminating()) {
            std::rethrow_exception(make_error());
        }
        int64_t sid = -1;
        int rv = kind == stream_kind::bidirectional
            ? ngtcp2_conn_open_bidi_stream(_conn, &sid, nullptr)
            : ngtcp2_conn_open_uni_stream(_conn, &sid, nullptr);
        if (rv == 0) {
            auto impl = make_lw_shared<quic_stream_impl>(shared_from_this(), sid, kind, _config.stream_send_buffer_size);
            _streams.emplace(sid, impl);
            co_return stream(std::move(impl));
        }
        if (rv != NGTCP2_ERR_STREAM_ID_BLOCKED) {
            throw std::system_error(make_error_code(errc::internal), ngtcp2_strerror(rv));
        }
        // Wait for the peer to raise MAX_STREAMS.
        auto& cv = kind == stream_kind::bidirectional ? _bidi_credit : _uni_credit;
        co_await cv.when();
    }
}

future<stream> quic_connection_impl::accept_stream() {
    auto impl = co_await _accept_q.pop_eventually();
    co_return stream(std::move(impl));
}

future<> quic_connection_impl::send_datagram(temporary_buffer<char> datagram) {
    if (!_config.enable_datagrams) {
        throw std::system_error(make_error_code(errc::unsupported), "DATAGRAM support not enabled");
    }
    if (is_terminating()) {
        std::rethrow_exception(make_error());
    }
    _tx_datagrams.emplace_back(std::move(datagram), promise<>());
    auto f = _tx_datagrams.back().second.get_future();
    _wake.signal();
    return f;
}

future<temporary_buffer<char>> quic_connection_impl::receive_datagram() {
    return _rx_datagrams.pop_eventually();
}

future<> quic_connection_impl::close(application_error_code code, sstring reason) noexcept {
    if (_state != state::closed && _state != state::closing && _state != state::draining && !_close_requested) {
        ngtcp2_ccerr_default(&_last_ccerr);
        // The reason phrase must stay alive until the CONNECTION_CLOSE
        // packet is written.
        _close_reason = std::move(reason);
        ngtcp2_ccerr_set_application_error(&_last_ccerr, code.value,
                                           reinterpret_cast<const uint8_t*>(_close_reason.data()),
                                           _close_reason.size());
        if (!_error) {
            _error = std::make_exception_ptr(std::system_error(make_error_code(errc::closing)));
        }
        _close_requested = true;
        _wake.signal();
    }
    return wait_closed();
}

future<> quic_connection_impl::wait_closed() noexcept {
    return _closed.get_shared_future();
}

std::optional<sstring> quic_connection_impl::alpn() const {
    return _tls->selected_alpn();
}

future<> quic_connection_impl::migrate(socket_address new_local, bool immediate) {
    if (_is_server) {
        throw std::system_error(make_error_code(errc::invalid_state), "only clients can migrate");
    }
    if (_state != state::established) {
        throw std::system_error(make_error_code(errc::invalid_state), "connection not established");
    }
    if (_path_validated) {
        throw std::system_error(make_error_code(errc::invalid_state), "migration already in progress");
    }
    // ngtcp2 only permits migration once the handshake is confirmed
    // (HANDSHAKE_DONE received) and a spare destination CID is available.
    while (!_handshake_confirmed && _state == state::established) {
        co_await _confirmed_cv.when();
    }
    if (_state != state::established) {
        throw std::system_error(make_error_code(errc::invalid_state), "connection not established");
    }

    auto new_channel = std::make_unique<quic_udp_channel>(new_local, !_config.disable_gso);
    // Retire the previous channel. It is closed (so its receive loop
    // exits) but kept alive in _retired_channels until the connection
    // ends, because that loop may still be suspended reading from it.
    // Packets in flight to the old address are lost, which QUIC recovers
    // from via retransmission.
    auto old = std::exchange(_own_channel, std::move(new_channel));
    _channel = _own_channel.get();
    _local_addr = _channel->local_address();
    old->close();
    _retired_channels.push_back(std::move(old));
    (void)client_receive_loop(_channel).handle_exception([] (std::exception_ptr) {
    }).finally([self = shared_from_this()] {});

    auto path = make_path(_local_addr, _remote_addr);
    int rv = immediate
        ? ngtcp2_conn_initiate_immediate_migration(_conn, &path, now())
        : ngtcp2_conn_initiate_migration(_conn, &path, now());
    if (rv != 0) {
        throw std::system_error(make_error_code(errc::internal), ngtcp2_strerror(rv));
    }
    _tx_pending = true; // PATH_CHALLENGE and probe packets to send
    _wake.signal();

    if (!immediate) {
        _path_validated.emplace();
        co_await _path_validated->get_future();
    }
}

future<session_ticket> quic_connection_impl::wait_for_session_ticket() {
    _ticket_waited = true;
    return _session_ticket.get_shared_future();
}

std::optional<address_token> quic_connection_impl::take_address_token() noexcept {
    return std::exchange(_token, std::nullopt);
}

connection_stats quic_connection_impl::get_stats() const noexcept {
    connection_stats stats{};
    if (_conn) {
        ngtcp2_conn_info info;
        ngtcp2_conn_get_conn_info2(_conn, &info);
        stats.smoothed_rtt = to_duration(info.smoothed_rtt);
        stats.cwnd = info.cwnd;
    }
    stats.bytes_sent = _bytes_sent;
    stats.bytes_received = _bytes_received;
    stats.forwarded_datagrams = _forwarded_datagrams;
    return stats;
}

// Stream-facing hooks.

void quic_connection_impl::extend_credit(stream_id id, size_t n) noexcept {
    if (is_terminating()) {
        return;
    }
    ngtcp2_conn_extend_max_stream_offset(_conn, id, n);
    ngtcp2_conn_extend_max_offset(_conn, n);
    _tx_pending = true; // MAX_STREAM_DATA / MAX_DATA frames to send
    _wake.signal();
}

// Removes the front of the send-ready queue, keeping the stream's
// membership flag in sync.
void quic_connection_impl::pop_send_ready() noexcept {
    _send_ready.front()->set_in_send_ready(false);
    _send_ready.pop_front();
}

void quic_connection_impl::notify_sendable(quic_stream_impl& s) {
    if (_state == state::closed) {
        return;
    }
    if (!s.in_send_ready()) {
        s.set_in_send_ready(true);
        _send_ready.push_back(s.shared_from_this());
    }
    _tx_pending = true;
    _wake.signal();
}

std::expected<void, std::error_code> quic_connection_impl::shutdown_stream_write(stream_id id, uint64_t code) noexcept {
    if (is_terminating()) {
        return std::unexpected(make_error_code(errc::closing));
    }
    if (auto rv = ngtcp2_conn_shutdown_stream_write(_conn, 0, id, code); rv != 0) {
        return std::unexpected(make_error_code(errc::stream_state));
    }
    _tx_pending = true; // RESET_STREAM frame to send
    _wake.signal();
    return {};
}

std::expected<void, std::error_code> quic_connection_impl::shutdown_stream_read(stream_id id, uint64_t code) noexcept {
    if (is_terminating()) {
        return std::unexpected(make_error_code(errc::closing));
    }
    if (auto rv = ngtcp2_conn_shutdown_stream_read(_conn, 0, id, code); rv != 0) {
        return std::unexpected(make_error_code(errc::stream_state));
    }
    _tx_pending = true; // STOP_SENDING frame to send
    _wake.signal();
    return {};
}

std::exception_ptr quic_connection_impl::make_error() const noexcept {
    if (_error) {
        return _error;
    }
    return std::make_exception_ptr(std::system_error(make_error_code(errc::closing)));
}

lw_shared_ptr<quic_stream_impl> quic_connection_impl::get_or_create_stream(stream_id id) {
    auto it = _streams.find(id);
    if (it != _streams.end()) {
        return it->second;
    }
    auto kind = (id & 0x2) ? stream_kind::unidirectional : stream_kind::bidirectional;
    auto impl = make_lw_shared<quic_stream_impl>(shared_from_this(), id, kind, _config.stream_send_buffer_size);
    _streams.emplace(id, impl);
    return impl;
}

quic_stream_impl* quic_connection_impl::find_stream(stream_id id) noexcept {
    auto it = _streams.find(id);
    return it == _streams.end() ? nullptr : it->second.get();
}

// ngtcp2 callbacks.

int quic_connection_impl::on_handshake_confirmed(ngtcp2_conn*, void* user_data) {
    auto& self = from(user_data);
    self._handshake_confirmed = true;
    self._confirmed_cv.broadcast();
    return 0;
}

int quic_connection_impl::on_handshake_completed(ngtcp2_conn*, void* user_data) {
    auto& self = from(user_data);
    self._state = state::established;
    self._tls->on_handshake_completed();
    if (!self._handshake_resolved) {
        self._handshake_resolved = true;
        self._handshake_done.set_value();
    }
    if (self._is_server && self._dispatcher) {
        self._dispatcher->connection_ready(self.shared_from_this());
    }
    return 0;
}

int quic_connection_impl::on_recv_stream_data(ngtcp2_conn*, uint32_t flags, int64_t stream_id, uint64_t,
                                              const uint8_t* data, size_t datalen, void* user_data, void*) {
    auto& self = from(user_data);
    auto stream = self.get_or_create_stream(stream_id);
    stream->on_data({data, datalen}, flags & NGTCP2_STREAM_DATA_FLAG_FIN);
    return 0;
}

int quic_connection_impl::on_acked_stream_data_offset(ngtcp2_conn*, int64_t stream_id, uint64_t offset,
                                                      uint64_t datalen, void* user_data, void*) {
    auto& self = from(user_data);
    if (auto* stream = self.find_stream(stream_id)) {
        stream->on_acked(offset + datalen);
    }
    return 0;
}

int quic_connection_impl::on_stream_open(ngtcp2_conn*, int64_t stream_id, void* user_data) {
    auto& self = from(user_data);
    auto stream = self.get_or_create_stream(stream_id);
    if (!self._accept_q.push(std::move(stream))) {
        // Bounded by the peer's stream credit, so this cannot normally
        // happen; treat as overload.
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int quic_connection_impl::on_stream_close(ngtcp2_conn*, uint32_t flags, int64_t stream_id,
                                          uint64_t app_error_code, void* user_data, void*) {
    auto& self = from(user_data);
    if (auto* stream = self.find_stream(stream_id)) {
        std::optional<uint64_t> code;
        if (flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET) {
            code = app_error_code;
        }
        stream->on_closed(code);
        self._streams.erase(stream_id);
    }
    // A closed peer-initiated stream returns one unit of stream credit to
    // the peer (MAX_STREAMS); ngtcp2 leaves this to the application. Without
    // it the peer runs out of streams after initial_max_streams_{bidi,uni}.
    if (!ngtcp2_conn_is_local_stream2(self._conn, stream_id)) {
        if (ngtcp2_is_bidi_stream(stream_id)) {
            ngtcp2_conn_extend_max_streams_bidi(self._conn, 1);
        } else {
            ngtcp2_conn_extend_max_streams_uni(self._conn, 1);
        }
        self._tx_pending = true; // MAX_STREAMS frame to send
        self._wake.signal();
    }
    return 0;
}

int quic_connection_impl::on_stream_reset(ngtcp2_conn*, int64_t stream_id, uint64_t, uint64_t app_error_code,
                                          void* user_data, void*) {
    auto& self = from(user_data);
    if (auto* stream = self.find_stream(stream_id)) {
        stream->on_reset(app_error_code);
    }
    return 0;
}

int quic_connection_impl::on_stream_stop_sending(ngtcp2_conn*, int64_t stream_id, uint64_t app_error_code,
                                                 void* user_data, void*) {
    auto& self = from(user_data);
    if (auto* stream = self.find_stream(stream_id)) {
        stream->on_stop_sending(app_error_code);
    }
    return 0;
}

int quic_connection_impl::on_extend_max_streams_bidi(ngtcp2_conn*, uint64_t, void* user_data) {
    from(user_data)._bidi_credit.broadcast();
    return 0;
}

int quic_connection_impl::on_extend_max_streams_uni(ngtcp2_conn*, uint64_t, void* user_data) {
    from(user_data)._uni_credit.broadcast();
    return 0;
}

int quic_connection_impl::on_extend_max_stream_data(ngtcp2_conn*, int64_t stream_id, uint64_t,
                                                    void* user_data, void*) {
    auto& self = from(user_data);
    if (auto* stream = self.find_stream(stream_id)) {
        stream->on_extend_max_stream_data();
        if (stream->wants_send()) {
            self.notify_sendable(*stream);
        }
    }
    return 0;
}

void quic_connection_impl::on_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*) {
    random_bytes({dest, destlen});
}

int quic_connection_impl::on_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid_out,
                                                   ngtcp2_stateless_reset_token* token,
                                                   size_t cidlen, void* user_data) {
    auto& self = from(user_data);
    cid c;
    if (self._is_server && self._dispatcher) {
        c = make_server_cid(self._dispatcher->secret(), this_shard_id());
        auto nc = c.to_ngtcp2();
        if (ngtcp2_crypto_generate_stateless_reset_token(token->data, self._dispatcher->secret().data.data(),
                                                         self._dispatcher->secret().data.size(),
                                                         &nc) != 0) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        self._dispatcher->associate_cid(c, self.shared_from_this());
        self._local_cids.push_back(c);
    } else {
        c.len = static_cast<uint8_t>(cidlen);
        random_bytes({c.data.data(), c.len});
        random_bytes({token->data, NGTCP2_STATELESS_RESET_TOKENLEN});
    }
    *cid_out = c.to_ngtcp2();
    return 0;
}

int quic_connection_impl::on_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid_in, void* user_data) {
    auto& self = from(user_data);
    auto c = cid::from(*cid_in);
    if (self._dispatcher) {
        self._dispatcher->dissociate_cid(c);
    }
    std::erase(self._local_cids, c);
    return 0;
}

int quic_connection_impl::on_path_validation(ngtcp2_conn*, uint32_t, const ngtcp2_path*,
                                             const ngtcp2_path*, ngtcp2_path_validation_result res,
                                             void* user_data) {
    auto& self = from(user_data);
    if (!self._path_validated) {
        return 0;
    }
    if (res == NGTCP2_PATH_VALIDATION_RESULT_SUCCESS) {
        self._path_validated->set_value();
    } else {
        self._path_validated->set_exception(std::make_exception_ptr(
            std::system_error(make_error_code(errc::path_validation_failed))));
    }
    self._path_validated.reset();
    return 0;
}

int quic_connection_impl::on_recv_datagram(ngtcp2_conn*, uint32_t, const uint8_t* data, size_t datalen,
                                           void* user_data) {
    auto& self = from(user_data);
    temporary_buffer<char> buf(reinterpret_cast<const char*>(data), datalen);
    // Datagrams are unreliable by contract: drop when the consumer lags.
    (void)self._rx_datagrams.push(std::move(buf));
    return 0;
}

int quic_connection_impl::on_recv_new_token(ngtcp2_conn*, const uint8_t* token, size_t tokenlen, void* user_data) {
    auto& self = from(user_data);
    self._token = address_token{std::vector<uint8_t>(token, token + tokenlen)};
    return 0;
}

int quic_connection_impl::on_recv_stateless_reset(ngtcp2_conn*, const ngtcp2_pkt_stateless_reset*, void* user_data) {
    auto& self = from(user_data);
    if (!self._error) {
        self._error = std::make_exception_ptr(std::system_error(make_error_code(errc::dropped_connection),
                                                                "stateless reset received"));
    }
    return 0;
}

} // namespace internal
} // namespace seastar::net::quic
