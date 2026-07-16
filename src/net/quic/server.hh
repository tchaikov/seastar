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
#include "connection.hh"
#include "udp_channel.hh"

#include <seastar/core/gate.hh>
#include <seastar/core/queue.hh>
#include <seastar/net/quic/quic.hh>

#include <unordered_map>

namespace seastar::net::quic::internal {

/// Per-shard QUIC server: owns the shard's SO_REUSEPORT UDP socket and
/// routes incoming datagrams to connections by destination CID.
///
/// Datagrams that miss the local CID map are either forwarded to the
/// shard embedded in the (server-generated) CID, answered with Version
/// Negotiation / Retry / stateless reset packets, or start a new
/// connection.
class server_dispatcher : public enable_lw_shared_from_this<server_dispatcher> {
public:
    server_dispatcher(socket_address addr, shared_ptr<tls::server_credentials> creds, listen_options options);
    ~server_dispatcher();

    void start();
    future<net::quic::connection> accept();
    void abort_accept();
    future<> stop(application_error_code code) noexcept;
    socket_address local_address() const {
        return _channel.local_address();
    }

    // Connection-facing.

    quic_udp_channel& channel() noexcept {
        return _channel;
    }
    const listen_options& options() const noexcept {
        return _options;
    }
    const server_secret& secret() const noexcept {
        return _secret;
    }
    /// Feeds a datagram forwarded from another shard (already known to
    /// route to a connection on this shard, or to start one here).
    void feed_forwarded(quic_udp_channel::rx_datagram dgram);
    void associate_cid(const cid& c, lw_shared_ptr<quic_connection_impl> conn) {
        _connections.emplace(c, std::move(conn));
    }
    void dissociate_cid(const cid& c) {
        _connections.erase(c);
    }
    /// Handshake completed: hand the connection to accept().
    void connection_ready(lw_shared_ptr<quic_connection_impl> conn) noexcept;

private:
    future<> receive_loop();
    future<> dispatch(quic_udp_channel::rx_datagram dgram);
    /// Handles a datagram that did not match any local connection.
    future<> handle_unmatched(quic_udp_channel::rx_datagram dgram, const ngtcp2_version_cid& vc);
    future<> send_reply(temporary_buffer<char> buf, size_t len, const quic_udp_channel::rx_datagram& dgram);
    future<> send_version_negotiation(const ngtcp2_version_cid& vc, const quic_udp_channel::rx_datagram& dgram);
    future<> send_retry(const ngtcp2_pkt_hd& hd, const quic_udp_channel::rx_datagram& dgram);
    future<> send_stateless_reset(const ngtcp2_version_cid& vc, const quic_udp_channel::rx_datagram& dgram);

    shared_ptr<tls::server_credentials> _creds;
    listen_options _options;
    quic_udp_channel _channel;
    server_secret _secret;
    std::unordered_map<cid, lw_shared_ptr<quic_connection_impl>, cid_hash> _connections;
    queue<net::quic::connection> _accept_q;
    unsigned _reset_budget = 100; // stateless resets per budget window
    lowres_clock::time_point _reset_budget_reset{};
    bool _stopping = false;
    future<> _receive_done = make_ready_future<>();
    gate _gate;
};

} // namespace seastar::net::quic::internal
