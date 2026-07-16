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

#ifndef SEASTAR_HAVE_QUIC
#error "<seastar/net/quic/types.hh> requires a Seastar build with Seastar_EXPERIMENTAL_QUIC enabled"
#endif

#include <cstdint>
#include <vector>

/// \defgroup quic-module QUIC
///
/// \brief Experimental QUIC (RFC 9000) transport support.
///
/// \note All interfaces in `seastar::net::quic` are experimental and may
/// change without notice.

namespace seastar::net::quic {

/// \addtogroup quic-module
/// @{

/// \brief A QUIC stream identifier (RFC 9000 §2.1).
///
/// The two least significant bits encode the initiator and directionality
/// of the stream. Negative values are never valid stream identifiers.
using stream_id = int64_t;

/// \brief Directionality of a QUIC stream (RFC 9000 §2.1).
enum class stream_kind {
    /// Both endpoints can send and receive on the stream.
    bidirectional,
    /// Only the stream initiator can send on the stream.
    unidirectional,
};

/// \brief A 62-bit application-defined error code.
///
/// Application error codes are carried by RESET_STREAM and STOP_SENDING
/// frames and by application-initiated CONNECTION_CLOSE frames. Their
/// meaning is defined by the application protocol in use (for example,
/// HTTP/3 defines the `H3_*` error codes in RFC 9114 §8.1).
struct application_error_code {
    /// The 62-bit error code value.
    uint64_t value = 0;
};

/// \brief An opaque TLS session ticket usable for session resumption.
///
/// Captured from a completed connection (see
/// \ref connection::wait_for_session_ticket()) and passed back via
/// \ref connect_options::ticket to resume a TLS session with the same
/// server, enabling abbreviated handshakes and (in the future) 0-RTT.
struct session_ticket {
    /// Backend-serialized TLS session data.
    std::vector<uint8_t> data;
};

/// \brief An address validation token received in a NEW_TOKEN frame.
///
/// May be presented in a future connection to the same server (via
/// \ref connect_options::token) to avoid an address-validation round trip.
struct address_token {
    /// The opaque token bytes.
    std::vector<uint8_t> data;
};

/// @}

} // namespace seastar::net::quic
