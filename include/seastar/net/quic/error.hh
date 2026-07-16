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

#include <seastar/net/quic/types.hh>
#include <seastar/core/sstring.hh>

#include <string>
#include <system_error>

namespace seastar::net::quic {

/// \addtogroup quic-module
/// @{

/// \brief Local QUIC error conditions.
///
/// These describe failures detected by the local QUIC implementation
/// (as opposed to error codes carried on the wire by the peer, for which
/// see \ref transport_error_category() and \ref application_error_category()).
/// Compare against `std::error_code` values obtained from failed futures
/// or from the `std::expected` returns of the synchronous entry points.
enum class errc {
    /// An internal error in the QUIC library.
    internal = 1,
    /// An invalid argument was supplied.
    invalid_argument,
    /// The operation is not valid in the current connection or stream state.
    invalid_state,
    /// The operation refers to a stream in the wrong state (e.g. writing
    /// to a stream whose sending side was already reset).
    stream_state,
    /// The TLS handshake failed.
    crypto_error,
    /// The connection was closed because the idle timeout expired.
    idle_close,
    /// The connection was dropped (e.g. a stateless reset was received).
    dropped_connection,
    /// Path validation of a migration target failed.
    path_validation_failed,
    /// The connection is closing or draining; no further I/O is possible.
    closing,
    /// The requested feature is not supported (e.g. DATAGRAM frames on a
    /// connection that did not negotiate them).
    unsupported,
    /// The operation was aborted locally.
    aborted,
};

/// \brief The `std::error_category` for \ref errc.
const std::error_category& error_category() noexcept;

/// \brief Makes a `std::error_code` from an \ref errc, in \ref error_category().
std::error_code make_error_code(errc e) noexcept;

/// \brief The category for QUIC transport error codes (RFC 9000 §20.1)
/// received in CONNECTION_CLOSE frames from the peer.
///
/// The `value()` of codes in this category is the (truncated) 62-bit wire
/// error code; use \ref connection_error::wire_code() for the full value.
const std::error_category& transport_error_category() noexcept;

/// \brief The category for application protocol error codes (RFC 9000 §20.2)
/// received in RESET_STREAM, STOP_SENDING and application CONNECTION_CLOSE
/// frames from the peer.
///
/// Application error code values are defined by the application protocol
/// (for example RFC 9114 §8.1 for HTTP/3); this category renders them
/// numerically.
const std::error_category& application_error_category() noexcept;

/// \brief Exception carried by failed futures when the connection was
/// closed by the peer or by the transport.
///
/// The embedded `std::error_code` is in \ref transport_error_category() or
/// \ref application_error_category() depending on the kind of
/// CONNECTION_CLOSE received; \ref wire_code() returns the untruncated
/// 62-bit error code and \ref reason() the peer's reason phrase, if any.
class connection_error : public std::system_error {
    uint64_t _wire_code;
    sstring _reason;
public:
    /// \brief Constructs from the closing error code and reason phrase.
    /// \param code the 62-bit wire error code
    /// \param application true if the code is in the application space
    /// \param reason the CONNECTION_CLOSE reason phrase (may be empty)
    connection_error(uint64_t code, bool application, sstring reason);

    /// \brief True if the connection was closed with an application error
    /// code, false if it was closed with a transport error code.
    bool is_application() const noexcept {
        return code().category() == application_error_category();
    }
    /// \brief The full 62-bit error code from the CONNECTION_CLOSE frame.
    uint64_t wire_code() const noexcept {
        return _wire_code;
    }
    /// \brief The reason phrase from the CONNECTION_CLOSE frame.
    std::string_view reason() const noexcept {
        return {_reason.begin(), _reason.size()};
    }
};

/// @}

} // namespace seastar::net::quic

namespace std {

template <>
struct is_error_code_enum<seastar::net::quic::errc> : true_type {};

} // namespace std
