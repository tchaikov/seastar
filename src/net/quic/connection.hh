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

#pragma once

#include "cid.hh"
#include "stream.hh"
#include "tls_session.hh"
#include "udp_channel.hh"

#include <seastar/core/abort_source.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/quic/quic.hh>
#include <seastar/util/log.hh>

#include <ngtcp2/ngtcp2.h>

#include <chrono>
#include <deque>
#include <unordered_map>

namespace seastar::net::quic {
extern logger quic_log;
}

namespace seastar::net::quic::internal {

class server_dispatcher;

/// The engine of one QUIC connection: owns the ngtcp2_conn, its TLS
/// session, and the single fiber that drives the RFC-mandated loop
/// (feed received datagrams -> handle expired timers -> produce packets
/// -> re-arm the expiry timer -> sleep).
///
/// All entry points must run on the owning shard.
class quic_connection_impl : public enable_lw_shared_from_this<quic_connection_impl> {
public:
    /// Client-side constructor path.
    static future<lw_shared_ptr<quic_connection_impl>> make_client(
        socket_address remote, shared_ptr<tls::certificate_credentials> creds, connect_options options);

    /// Server-side constructor path; invoked by the dispatcher on an
    /// acceptable Initial packet. \p odcid is the client's original
    /// destination CID (recovered from the retry token when a Retry was
    /// performed, in which case \p retry_scid is the CID the Retry
    /// carried); \p token/\p token_type describe a validated address
    /// token, if any.
    static future<lw_shared_ptr<quic_connection_impl>> make_server(
        lw_shared_ptr<server_dispatcher> dispatcher, shared_ptr<tls::server_credentials> creds,
        const connection_config& config, const ngtcp2_pkt_hd& hd,
        const ngtcp2_cid& odcid, const ngtcp2_cid* retry_scid,
        std::span<const uint8_t> token, ngtcp2_token_type token_type,
        const quic_udp_channel::rx_datagram& first_dgram);

    quic_connection_impl();
    ~quic_connection_impl();
    quic_connection_impl(const quic_connection_impl&) = delete;

    // Public API backing net::quic::connection.

    future<stream> open_stream(stream_kind kind);
    future<stream> accept_stream();
    future<> send_datagram(temporary_buffer<char> datagram);
    future<temporary_buffer<char>> receive_datagram();
    future<> close(application_error_code code, sstring reason) noexcept;
    future<> wait_closed() noexcept;
    /// True once the connection is shutting down (closing, draining or
    /// closed) and no new work may be started on it.
    bool is_terminating() const noexcept {
        return _state == state::closed || _state == state::closing || _state == state::draining;
    }
    bool is_closed() const noexcept {
        return _state == state::closed;
    }
    std::optional<sstring> alpn() const;
    socket_address local_address() const {
        return _local_addr;
    }
    socket_address remote_address() const {
        return _remote_addr;
    }
    future<> migrate(socket_address new_local, bool immediate);
    future<session_ticket> wait_for_session_ticket();
    std::optional<address_token> take_address_token() noexcept;
    connection_stats get_stats() const noexcept;

    /// Resolves when the handshake completes (fails if the connection
    /// dies first).
    future<> wait_handshake() {
        return _handshake_done.get_shared_future();
    }

    // Stream-facing hooks.

    /// Grants stream + connection level flow-control credit.
    void extend_credit(stream_id id, size_t n) noexcept;
    /// Marks a stream as having data to send and wakes the fiber.
    void notify_sendable(quic_stream_impl& s);
    void pop_send_ready() noexcept;
    std::expected<void, std::error_code> shutdown_stream_write(stream_id id, uint64_t app_error_code) noexcept;
    std::expected<void, std::error_code> shutdown_stream_read(stream_id id, uint64_t app_error_code) noexcept;
    const connection_config& config() const noexcept {
        return _config;
    }
    bool is_server() const noexcept {
        return _is_server;
    }
    // Test-only: simulates a fatal, unhandled channel failure (e.g.
    // sendmsg() returning an errno outside quic_udp_channel's own
    // GSO-fallback handling) by shutting the channel down out from under a
    // live connection, so the next write_packets() send fails the way a
    // real transport error would. Internal-only (not reachable through the
    // public API); exists to let tests exercise run()'s failure path.
    void break_channel_for_testing() noexcept {
        _channel->close();
    }
    /// A never-failing exception describing why the connection ended.
    std::exception_ptr make_error() const noexcept;
    void wake() noexcept {
        _wake.signal();
    }

    // Dispatcher-facing hooks.

    /// Feeds one received (non-coalesced) UDP datagram to the connection.
    void feed_datagram(quic_udp_channel::rx_datagram dgram, bool forwarded = false);
    /// All connection IDs currently routing to this connection.
    const std::vector<cid>& local_cids() const noexcept {
        return _local_cids;
    }

private:
    enum class state {
        handshaking,
        established,
        closing,   // we sent CONNECTION_CLOSE, waiting out the close period
        draining,  // peer sent CONNECTION_CLOSE
        closed,
    };

    static ngtcp2_tstamp now() noexcept {
        return quic_now();
    }
    static ngtcp2_callbacks make_callbacks(bool server) noexcept;
    static quic_connection_impl& from(void* user_data) noexcept {
        return *static_cast<quic_connection_impl*>(user_data);
    }
    static ngtcp2_conn* get_conn_cb(ngtcp2_crypto_conn_ref* ref) noexcept;

    void setup_settings_and_params(ngtcp2_settings& settings, ngtcp2_transport_params& params) const;
    void start();                         // spawn the fibers
    future<> run();                       // the connection fiber
    // run()'s body, wrapped by run() in a catch-all that guarantees
    // finish() runs even if a failure escapes the loop uncaught.
    future<> run_loop();
    future<> client_receive_loop(quic_udp_channel* channel); // client-only channel reader
    void process_rx();                    // drain _rx through ngtcp2_conn_read_pkt
    ngtcp2_ssize write_one_packet(ngtcp2_path_storage& ps, ngtcp2_pkt_info& pi, uint8_t* dest, size_t destlen);
    future<> write_packets();             // produce and send packet batches
    void rearm_timer();
    void enter_draining();
    future<> send_connection_close();
    bool _expiry_due() const noexcept;
    void handle_ngtcp2_failure(int rv);   // fatal library error -> closing
    future<> finish();                    // tear everything down
    lw_shared_ptr<quic_stream_impl> get_or_create_stream(stream_id id);
    quic_stream_impl* find_stream(stream_id id) noexcept;

    // ngtcp2 callbacks (static trampolines)
    static int on_handshake_confirmed(ngtcp2_conn*, void* user_data);
    static int on_handshake_completed(ngtcp2_conn*, void* user_data);
    static int on_recv_stream_data(ngtcp2_conn*, uint32_t flags, int64_t stream_id, uint64_t offset,
                                   const uint8_t* data, size_t datalen, void* user_data, void* stream_user_data);
    static int on_acked_stream_data_offset(ngtcp2_conn*, int64_t stream_id, uint64_t offset, uint64_t datalen,
                                           void* user_data, void* stream_user_data);
    static int on_stream_open(ngtcp2_conn*, int64_t stream_id, void* user_data);
    static int on_stream_close(ngtcp2_conn*, uint32_t flags, int64_t stream_id, uint64_t app_error_code,
                               void* user_data, void* stream_user_data);
    static int on_stream_reset(ngtcp2_conn*, int64_t stream_id, uint64_t final_size, uint64_t app_error_code,
                               void* user_data, void* stream_user_data);
    static int on_stream_stop_sending(ngtcp2_conn*, int64_t stream_id, uint64_t app_error_code,
                                      void* user_data, void* stream_user_data);
    static int on_extend_max_streams_bidi(ngtcp2_conn*, uint64_t max_streams, void* user_data);
    static int on_extend_max_streams_uni(ngtcp2_conn*, uint64_t max_streams, void* user_data);
    static int on_extend_max_stream_data(ngtcp2_conn*, int64_t stream_id, uint64_t max_data,
                                         void* user_data, void* stream_user_data);
    static void on_rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*);
    static int on_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token,
                                        size_t cidlen, void* user_data);
    static int on_remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data);
    static int on_path_validation(ngtcp2_conn*, uint32_t flags, const ngtcp2_path* path,
                                  const ngtcp2_path* old_path, ngtcp2_path_validation_result res, void* user_data);
    static int on_recv_datagram(ngtcp2_conn*, uint32_t flags, const uint8_t* data, size_t datalen, void* user_data);
    static int on_recv_new_token(ngtcp2_conn*, const uint8_t* token, size_t tokenlen, void* user_data);
    static int on_recv_stateless_reset(ngtcp2_conn*, const ngtcp2_pkt_stateless_reset*, void* user_data);

    friend class server_dispatcher;

    ngtcp2_conn* _conn = nullptr;
    conn_ref_holder _holder{};
    std::unique_ptr<tls_session> _tls;
    connection_config _config;
    bool _is_server = false;

    // I/O: clients own their channel; server connections share the
    // dispatcher's.
    std::unique_ptr<quic_udp_channel> _own_channel;
    quic_udp_channel* _channel = nullptr;
    lw_shared_ptr<server_dispatcher> _dispatcher;
    // Channels retired by migration: closed but kept alive until the
    // connection ends, so their in-flight receive loops never touch a
    // freed object.
    std::vector<std::unique_ptr<quic_udp_channel>> _retired_channels;
    socket_address _local_addr;
    socket_address _remote_addr;

    std::deque<quic_udp_channel::rx_datagram> _rx;
    condition_variable _wake;
    timer<std::chrono::steady_clock> _expiry_timer;
    // An event since the last write pass may let ngtcp2 produce packets
    // (new stream data, window extension, RESET_STREAM/STOP_SENDING,
    // migration). The connection fiber's wake predicate uses this instead
    // of `!_send_ready.empty()`: queued-but-blocked stream data must not
    // keep the fiber spinning while ngtcp2 is congestion- or pacer-limited
    // (progress then comes from received ACKs or the expiry timer, whose
    // deadline includes the pacer).
    bool _tx_pending = true;

    std::unordered_map<stream_id, lw_shared_ptr<quic_stream_impl>> _streams;
    queue<lw_shared_ptr<quic_stream_impl>> _accept_q{128};
    std::deque<lw_shared_ptr<quic_stream_impl>> _send_ready;
    condition_variable _bidi_credit;
    condition_variable _uni_credit;

    queue<temporary_buffer<char>> _rx_datagrams{128};
    std::deque<std::pair<temporary_buffer<char>, promise<>>> _tx_datagrams;

    state _state = state::handshaking;
    shared_promise<> _handshake_done;
    bool _handshake_resolved = false;
    bool _handshake_confirmed = false;
    condition_variable _confirmed_cv;
    shared_promise<> _closed;
    bool _finished = false;
    std::optional<promise<>> _path_validated;
    shared_promise<session_ticket> _session_ticket;
    bool _ticket_resolved = false;
    bool _ticket_waited = false;
    std::optional<address_token> _token;
    std::exception_ptr _error;
    ngtcp2_ccerr _last_ccerr{};
    sstring _close_reason;
    bool _close_requested = false;
    std::chrono::steady_clock::time_point _close_deadline{};

    std::vector<cid> _local_cids;
    uint64_t _forwarded_datagrams = 0;
    uint64_t _bytes_sent = 0;
    uint64_t _bytes_received = 0;
    gate _gate;
};

} // namespace seastar::net::quic::internal
