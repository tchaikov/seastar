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

#include <seastar/core/future.hh>
#include <seastar/core/internal/pollable_fd.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/socket_defs.hh>

#include <cstdint>
#include <optional>

namespace seastar::net::quic::internal {

/// A kernel UDP socket tailored for QUIC I/O, wrapping a raw pollable_fd.
///
/// Compared to net::datagram_channel it additionally provides:
///  - segmented sends via UDP GSO (UDP_SEGMENT), with automatic latching
///    fallback to per-segment sendmsg() when the kernel or path rejects it
///  - receive offload via UDP GRO (UDP_GRO); a received buffer may contain
///    several coalesced datagrams of gro_segment_size bytes each
///  - ECN marking on both directions (IP_TOS / IPV6_TCLASS)
///  - destination address recovery via IP_PKTINFO / IPV6_RECVPKTINFO
///
/// The socket is bound at construction (SO_REUSEPORT when available, so
/// every shard can bind the same address) and never connected; each send
/// names its destination.
class quic_udp_channel {
public:
    /// One received kernel datagram, possibly GRO-coalesced.
    struct rx_datagram {
        temporary_buffer<char> data;
        socket_address src;
        /// Destination address of the datagram (from pktinfo); the port is
        /// the channel's bound port.
        socket_address dst;
        /// ECN bits from the IP TOS / traffic class field.
        uint8_t ecn = 0;
        /// When non-zero, \c data holds several consecutive datagrams of
        /// this size each (the last one may be shorter).
        uint16_t gro_segment_size = 0;
    };

    /// A batch of equally-sized QUIC packets laid out back to back,
    /// destined for a single peer. The last segment may be shorter.
    struct tx_batch {
        temporary_buffer<char> data;
        socket_address dst;
        /// Size of each segment; must equal data.size() for a single
        /// packet, in which case no GSO is involved.
        size_t segment_size = 0;
        /// ECN bits to set in the IP TOS / traffic class field (0 = none).
        uint8_t ecn = 0;
        /// Source address for the IP pktinfo control message; used by
        /// servers bound to a wildcard address to reply from the address
        /// the peer contacted.
        std::optional<socket_address> src;
    };

    /// Binds a UDP socket to \p local (port 0 picks an ephemeral port).
    /// \param enable_gso probe for and use UDP_SEGMENT on sends
    /// \param enable_gro enable UDP_GRO on receives
    explicit quic_udp_channel(const socket_address& local, bool enable_gso = true, bool enable_gro = true);

    quic_udp_channel(const quic_udp_channel&) = delete;
    quic_udp_channel& operator=(const quic_udp_channel&) = delete;

    /// Receives one kernel datagram (which may be GRO-coalesced).
    future<rx_datagram> receive();

    /// Sends a batch. Uses one GSO sendmsg() when the batch holds several
    /// segments and GSO is operational; otherwise sends the segments
    /// one at a time. A kernel rejection of GSO (EIO/EINVAL/ENOTSUP)
    /// permanently disables it for this channel and resends segment-wise.
    future<> send(tx_batch batch);

    /// True if segmented sends currently use UDP GSO.
    bool gso_enabled() const noexcept {
        return _gso_available && !_gso_broken;
    }

    /// Largest number of segments worth batching into one send() call.
    size_t max_gso_segments() const noexcept {
        return gso_enabled() ? max_segments_per_batch : 1;
    }

    socket_address local_address() const noexcept {
        return _address;
    }

    void close() noexcept;

    bool is_closed() const noexcept {
        return _closed;
    }

private:
    static constexpr size_t max_segments_per_batch = 10;
    // Big enough for a maximally GRO-coalesced receive.
    static constexpr size_t recv_buffer_size = 65536;

    future<> send_one(const socket_address& dst, const std::optional<socket_address>& src,
                      const char* data, size_t len, size_t segment_size, uint8_t ecn);

    pollable_fd _fd;
    socket_address _address;
    // Serializes sends: the channel is shared by every connection on the
    // shard (the server binds one socket per shard), and each connection
    // fiber sends independently. A blocked sendmsg() parks on the fd's
    // single poll_write slot, so two concurrent senders would double-arm
    // that slot; one in-flight send at a time keeps them ordered.
    semaphore _tx_sem{1};
    bool _gso_available = false;
    bool _gso_broken = false;
    bool _gro_enabled = false;
    bool _closed = false;
};

} // namespace seastar::net::quic::internal
