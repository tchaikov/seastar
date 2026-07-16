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

#include <seastar/net/quic/error.hh>

#include <fmt/format.h>

#include <climits>
#include <utility>

namespace seastar::net::quic {

namespace {

class quic_error_category final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "quic";
    }
    std::string message(int ev) const override {
        switch (static_cast<errc>(ev)) {
        case errc::internal:
            return "internal QUIC error";
        case errc::invalid_argument:
            return "invalid argument";
        case errc::invalid_state:
            return "operation invalid in current state";
        case errc::stream_state:
            return "invalid stream state";
        case errc::crypto_error:
            return "TLS handshake error";
        case errc::idle_close:
            return "connection closed on idle timeout";
        case errc::dropped_connection:
            return "connection dropped";
        case errc::path_validation_failed:
            return "path validation failed";
        case errc::closing:
            return "connection is closing";
        case errc::unsupported:
            return "operation not supported";
        case errc::aborted:
            return "operation aborted";
        }
        return fmt::format("unknown QUIC error {}", ev);
    }
};

class quic_transport_error_category final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "quic:transport";
    }
    std::string message(int ev) const override {
        // RFC 9000 §20.1 transport error codes.
        switch (ev) {
        case 0x00: return "NO_ERROR";
        case 0x01: return "INTERNAL_ERROR";
        case 0x02: return "CONNECTION_REFUSED";
        case 0x03: return "FLOW_CONTROL_ERROR";
        case 0x04: return "STREAM_LIMIT_ERROR";
        case 0x05: return "STREAM_STATE_ERROR";
        case 0x06: return "FINAL_SIZE_ERROR";
        case 0x07: return "FRAME_ENCODING_ERROR";
        case 0x08: return "TRANSPORT_PARAMETER_ERROR";
        case 0x09: return "CONNECTION_ID_LIMIT_ERROR";
        case 0x0a: return "PROTOCOL_VIOLATION";
        case 0x0b: return "INVALID_TOKEN";
        case 0x0c: return "APPLICATION_ERROR";
        case 0x0d: return "CRYPTO_BUFFER_EXCEEDED";
        case 0x0e: return "KEY_UPDATE_ERROR";
        case 0x0f: return "AEAD_LIMIT_REACHED";
        case 0x10: return "NO_VIABLE_PATH";
        }
        if (ev >= 0x0100 && ev <= 0x01ff) {
            return fmt::format("CRYPTO_ERROR (TLS alert {})", ev - 0x0100);
        }
        return fmt::format("transport error {:#x}", ev);
    }
};

class quic_application_error_category final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "quic:application";
    }
    std::string message(int ev) const override {
        return fmt::format("application error {:#x}", ev);
    }
};

// Wire error codes are 62 bits wide, but std::error_code carries an int.
// Values that do not fit are clamped; connection_error::wire_code()
// preserves the full value.
constexpr int clamp_wire_code(uint64_t code) noexcept {
    return code > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(code);
}

} // anonymous namespace

const std::error_category& error_category() noexcept {
    static const quic_error_category category;
    return category;
}

const std::error_category& transport_error_category() noexcept {
    static const quic_transport_error_category category;
    return category;
}

const std::error_category& application_error_category() noexcept {
    static const quic_application_error_category category;
    return category;
}

std::error_code make_error_code(errc e) noexcept {
    return {std::to_underlying(e), error_category()};
}

connection_error::connection_error(uint64_t code, bool application, sstring reason)
    : std::system_error(clamp_wire_code(code),
                        application ? application_error_category() : transport_error_category(),
                        fmt::format("connection closed{}{}", reason.empty() ? "" : ": ", reason))
    , _wire_code(code)
    , _reason(std::move(reason)) {
}

} // namespace seastar::net::quic
