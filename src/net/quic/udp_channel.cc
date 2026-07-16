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

#include "udp_channel.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/assert.hh>

#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace seastar::net::quic::internal {

namespace {

constexpr bool is_inet(sa_family_t family) noexcept {
    return family == AF_INET || family == AF_INET6;
}

file_desc create_socket(socket_address local) {
    auto family = local.family();
    if (!is_inet(family)) {
        throw std::invalid_argument("QUIC requires an IPv4 or IPv6 address");
    }
    file_desc fd = file_desc::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (family == AF_INET) {
        fd.setsockopt(SOL_IP, IP_PKTINFO, true);
        fd.setsockopt(SOL_IP, IP_RECVTOS, true);
    } else {
        fd.setsockopt(SOL_IPV6, IPV6_RECVPKTINFO, true);
        fd.setsockopt(SOL_IPV6, IPV6_RECVTCLASS, true);
    }
    if (engine().posix_reuseport_available()) {
        fd.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    }
    // Enlarge the socket buffers. QUIC does its own loss recovery and
    // congestion control, but a receive buffer overrun still shows up as
    // packet loss, which collapses the congestion window: on fast paths a
    // sender can burst far more than the ~208KB default holds before the
    // reactor drains it. Request a generous size (the kernel silently caps
    // it at net.core.{r,w}mem_max); this is best-effort, so ignore errors.
    int bufsz = 8 * 1024 * 1024;
    try {
        fd.setsockopt(SOL_SOCKET, SO_RCVBUF, bufsz);
        fd.setsockopt(SOL_SOCKET, SO_SNDBUF, bufsz);
    } catch (...) {
    }
    fd.bind(local.u.sa, local.addr_length);
    return fd;
}

} // anonymous namespace

quic_udp_channel::quic_udp_channel(const socket_address& local, bool enable_gso, bool enable_gro) {
    auto fd = create_socket(local);
#ifdef UDP_SEGMENT
    if (enable_gso) {
        // Probe support; the actual segment size travels as a per-send cmsg.
        int off = 0;
        _gso_available = ::setsockopt(fd.get(), SOL_UDP, UDP_SEGMENT, &off, sizeof(off)) == 0;
    }
#endif
#ifdef UDP_GRO
    if (enable_gro) {
        int on = 1;
        _gro_enabled = ::setsockopt(fd.get(), SOL_UDP, UDP_GRO, &on, sizeof(on)) == 0;
    }
#endif
    _address = fd.get_address();
    _fd = pollable_fd(std::move(fd));
}

void quic_udp_channel::close() noexcept {
    _closed = true;
    _fd = {};
}

future<quic_udp_channel::rx_datagram> quic_udp_channel::receive() {
    auto buf = temporary_buffer<char>(recv_buffer_size);
    socket_address src{};
    // Room for pktinfo, TOS/TCLASS and GRO control messages.
    alignas(struct cmsghdr) char cbuf[CMSG_SPACE(sizeof(struct in6_pktinfo)) + 3 * CMSG_SPACE(sizeof(int))];
    struct iovec iov{buf.get_write(), recv_buffer_size};
    struct msghdr hdr{};
    hdr.msg_name = &src.u.sa;
    hdr.msg_namelen = sizeof(src.u.sas);
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = cbuf;
    hdr.msg_controllen = sizeof(cbuf);

    auto size = co_await _fd.recvmsg(&hdr);

    rx_datagram result;
    result.src = src;
    result.dst = _address;
    for (auto* cmsg = CMSG_FIRSTHDR(&hdr); cmsg != nullptr; cmsg = CMSG_NXTHDR(&hdr, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo pi;
            std::memcpy(&pi, CMSG_DATA(cmsg), sizeof(pi));
            result.dst = ipv4_addr(pi.ipi_addr, _address.port());
        } else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo pi;
            std::memcpy(&pi, CMSG_DATA(cmsg), sizeof(pi));
            result.dst = ipv6_addr(pi.ipi6_addr, _address.port());
        } else if ((cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TOS)
                   || (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_TCLASS)) {
            // IP_TOS arrives as a single byte, IPV6_TCLASS as an int.
            if (cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
                int tclass;
                std::memcpy(&tclass, CMSG_DATA(cmsg), sizeof(tclass));
                result.ecn = static_cast<uint8_t>(tclass) & 0x03;
            } else {
                result.ecn = *reinterpret_cast<const uint8_t*>(CMSG_DATA(cmsg)) & 0x03;
            }
        }
#ifdef UDP_GRO
        else if (cmsg->cmsg_level == SOL_UDP && cmsg->cmsg_type == UDP_GRO) {
            int seg;
            std::memcpy(&seg, CMSG_DATA(cmsg), sizeof(seg));
            if (seg > 0 && static_cast<size_t>(seg) < size) {
                result.gro_segment_size = static_cast<uint16_t>(seg);
            }
        }
#endif
    }
    buf.trim(size);
    result.data = std::move(buf);
    co_return result;
}

future<> quic_udp_channel::send_one(const socket_address& dst, const std::optional<socket_address>& src,
                                    const char* data, size_t len, size_t segment_size, uint8_t ecn) {
    socket_address name = dst;
    struct iovec iov{const_cast<char*>(data), len};
    struct msghdr hdr{};
    hdr.msg_name = &name.u.sa;
    hdr.msg_namelen = name.addr_length;
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;

    alignas(struct cmsghdr) char cbuf[CMSG_SPACE(sizeof(uint16_t)) + CMSG_SPACE(sizeof(int))
                                      + CMSG_SPACE(sizeof(struct in6_pktinfo))] = {};
    hdr.msg_control = cbuf;
    hdr.msg_controllen = sizeof(cbuf);
    auto* cmsg = CMSG_FIRSTHDR(&hdr);
    size_t controllen = 0;
    auto add_cmsg = [&](int level, int type, const void* value, size_t value_len) {
        cmsg->cmsg_level = level;
        cmsg->cmsg_type = type;
        cmsg->cmsg_len = CMSG_LEN(value_len);
        std::memcpy(CMSG_DATA(cmsg), value, value_len);
        controllen += CMSG_SPACE(value_len);
        cmsg = CMSG_NXTHDR(&hdr, cmsg);
    };

#ifdef UDP_SEGMENT
    if (segment_size > 0 && segment_size < len) {
        auto seg = static_cast<uint16_t>(segment_size);
        add_cmsg(SOL_UDP, UDP_SEGMENT, &seg, sizeof(seg));
    }
#endif
    if (ecn != 0) {
        int tclass = ecn;
        if (dst.family() == AF_INET) {
            add_cmsg(IPPROTO_IP, IP_TOS, &tclass, sizeof(tclass));
        } else {
            add_cmsg(IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
        }
    }
    if (src && is_inet(src->family()) && !src->is_wildcard()) {
        if (src->family() == AF_INET) {
            struct in_pktinfo pi{};
            pi.ipi_spec_dst = src->as_posix_sockaddr_in().sin_addr;
            add_cmsg(IPPROTO_IP, IP_PKTINFO, &pi, sizeof(pi));
        } else {
            struct in6_pktinfo pi{};
            pi.ipi6_addr = src->as_posix_sockaddr_in6().sin6_addr;
            add_cmsg(IPPROTO_IPV6, IPV6_PKTINFO, &pi, sizeof(pi));
        }
    }
    if (controllen == 0) {
        hdr.msg_control = nullptr;
    }
    hdr.msg_controllen = controllen;

    auto sent = co_await _fd.sendmsg(&hdr);
    SEASTAR_ASSERT(sent == len);
}

future<> quic_udp_channel::send(tx_batch batch) {
    if (batch.data.empty()) {
        co_return;
    }
    // Hold the tx slot for the whole send: on a full socket buffer sendmsg()
    // parks on the fd's single poll_write completion, which must not be
    // shared by concurrent senders on this per-shard channel.
    auto units = co_await get_units(_tx_sem, 1);
    auto seg = batch.segment_size == 0 ? batch.data.size() : batch.segment_size;
    if (seg >= batch.data.size()) {
        // Single packet; no segmentation involved.
        co_await send_one(batch.dst, batch.src, batch.data.get(), batch.data.size(), 0, batch.ecn);
        co_return;
    }
    if (gso_enabled()) {
        try {
            co_await send_one(batch.dst, batch.src, batch.data.get(), batch.data.size(), seg, batch.ecn);
            co_return;
        } catch (const std::system_error& e) {
            auto err = e.code().value();
            if (err != EIO && err != EINVAL && err != ENOTSUP && err != EOPNOTSUPP) {
                throw;
            }
            // The kernel or the egress path rejected GSO (observed with some
            // drivers and tunnel devices); disable it for good and fall back
            // to sending the segments individually.
            _gso_broken = true;
        }
    }
    for (size_t off = 0; off < batch.data.size(); off += seg) {
        auto len = std::min(seg, batch.data.size() - off);
        co_await send_one(batch.dst, batch.src, batch.data.get() + off, len, 0, batch.ecn);
    }
}

} // namespace seastar::net::quic::internal
