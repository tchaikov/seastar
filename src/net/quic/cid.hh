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

#include <ngtcp2/ngtcp2.h>

#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace seastar::net::quic::internal {

/// Length of server-generated connection IDs:
/// two shard-id bytes (masked) followed by 16 random bytes.
inline constexpr uint8_t server_cid_len = 18;

/// A QUIC connection ID (RFC 9000 §5.1).
struct cid {
    std::array<uint8_t, NGTCP2_MAX_CIDLEN> data{};
    uint8_t len = 0;

    static cid from(const uint8_t* p, size_t n) {
        cid c;
        if (n > c.data.size()) {
            throw std::invalid_argument("connection ID too long");
        }
        std::memcpy(c.data.data(), p, n);
        c.len = static_cast<uint8_t>(n);
        return c;
    }

    static cid from(const ngtcp2_cid& nc) {
        return from(nc.data, nc.datalen);
    }

    ngtcp2_cid to_ngtcp2() const {
        ngtcp2_cid nc;
        ngtcp2_cid_init(&nc, data.data(), len);
        return nc;
    }

    std::span<const uint8_t> bytes() const {
        return {data.data(), len};
    }

    bool operator==(const cid& o) const {
        return std::ranges::equal(bytes(), o.bytes());
    }
};

struct cid_hash {
    size_t operator()(const cid& c) const {
        // Server CIDs carry 16 random bytes after the shard prefix, and
        // client-chosen CIDs are random too; the tail bytes hash well.
        uint64_t v = 0;
        std::memcpy(&v, c.data.data() + (c.len >= 8 ? c.len - 8 : 0), std::min<size_t>(c.len, 8));
        return v ^ c.len;
    }
};

/// Fills a buffer with kernel-provided randomness.
inline void random_bytes(std::span<uint8_t> out) {
    size_t done = 0;
    while (done < out.size()) {
        auto n = ::getrandom(out.data() + done, out.size() - done, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::system_category(), "getrandom");
        }
        done += n;
    }
}

/// Per-server secret material, generated once per listen() and replicated
/// to every shard. Feeds the CID shard mask, retry/address-validation
/// tokens and stateless reset tokens.
struct server_secret {
    std::array<uint8_t, 32> data{};

    static server_secret generate() {
        server_secret s;
        random_bytes(s.data);
        return s;
    }
};

/// Generates a fresh server connection ID with this shard's id embedded
/// in the (masked) first two bytes.
inline cid make_server_cid(const server_secret& secret, uint16_t shard) {
    cid c;
    c.len = server_cid_len;
    random_bytes(std::span{c.data}.subspan(2, server_cid_len - 2));
    uint16_t masked = shard ^ static_cast<uint16_t>((secret.data[0] << 8) | secret.data[1]);
    c.data[0] = static_cast<uint8_t>(masked >> 8);
    c.data[1] = static_cast<uint8_t>(masked & 0xff);
    return c;
}

/// Recovers the shard id embedded by make_server_cid(). Only meaningful
/// for connection IDs this server generated; for anything else the result
/// is an arbitrary value that the caller must range-check.
inline uint16_t shard_of_cid(const server_secret& secret, std::span<const uint8_t> cid_bytes) noexcept {
    if (cid_bytes.size() < 2) {
        return UINT16_MAX;
    }
    uint16_t masked = static_cast<uint16_t>((cid_bytes[0] << 8) | cid_bytes[1]);
    return masked ^ static_cast<uint16_t>((secret.data[0] << 8) | secret.data[1]);
}

} // namespace seastar::net::quic::internal
