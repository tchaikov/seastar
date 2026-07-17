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
#error "<seastar/http3/error.hh> requires a Seastar build with Seastar_EXPERIMENTAL_QUIC enabled"
#endif

#include <seastar/core/sstring.hh>

#include <cstdint>
#include <system_error>

/// \defgroup http3-module HTTP/3
///
/// \brief Experimental HTTP/3 (RFC 9114) support over the QUIC transport.
///
/// \note All interfaces in `seastar::experimental::http3` are experimental
/// and may change without notice.

namespace seastar::experimental::http3 {

/// \addtogroup http3-module
/// @{

/// \brief HTTP/3 error codes (RFC 9114 §8.1).
///
/// These 62-bit application error codes are used on QUIC RESET_STREAM,
/// STOP_SENDING and application CONNECTION_CLOSE frames.
enum class h3_error_code : uint64_t {
    no_error = 0x0100,
    general_protocol_error = 0x0101,
    internal_error = 0x0102,
    stream_creation_error = 0x0103,
    closed_critical_stream = 0x0104,
    frame_unexpected = 0x0105,
    frame_error = 0x0106,
    excessive_load = 0x0107,
    id_error = 0x0108,
    settings_error = 0x0109,
    missing_settings = 0x010a,
    request_rejected = 0x010b,
    request_cancelled = 0x010c,
    request_incomplete = 0x010d,
    message_error = 0x010e,
    connect_error = 0x010f,
    version_fallback = 0x0110,
};

/// \brief The `std::error_category` for \ref h3_error_code.
const std::error_category& http3_error_category() noexcept;

/// \brief Makes a `std::error_code` from an \ref h3_error_code.
std::error_code make_error_code(h3_error_code e) noexcept;

/// \brief Exception thrown (via failed futures) for HTTP/3 protocol
/// errors and failed requests.
class http3_exception : public std::system_error {
public:
    /// \brief Constructs from an \ref h3_error_code and optional message.
    explicit http3_exception(h3_error_code code, const sstring& what = {})
        : std::system_error(make_error_code(code), std::string(what.begin(), what.end())) {
    }
    /// \brief Constructs from an arbitrary `std::error_code` and optional
    /// message.
    explicit http3_exception(std::error_code code, const sstring& what = {})
        : std::system_error(code, std::string(what.begin(), what.end())) {
    }
};

/// @}

} // namespace seastar::experimental::http3

namespace std {

template <>
struct is_error_code_enum<seastar::experimental::http3::h3_error_code> : true_type {};

} // namespace std
