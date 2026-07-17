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

#include <seastar/http3/error.hh>

#include <fmt/format.h>

namespace seastar::experimental::http3 {

namespace {

class h3_error_category final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "http3";
    }
    std::string message(int ev) const override {
        switch (static_cast<h3_error_code>(ev)) {
        case h3_error_code::no_error: return "H3_NO_ERROR";
        case h3_error_code::general_protocol_error: return "H3_GENERAL_PROTOCOL_ERROR";
        case h3_error_code::internal_error: return "H3_INTERNAL_ERROR";
        case h3_error_code::stream_creation_error: return "H3_STREAM_CREATION_ERROR";
        case h3_error_code::closed_critical_stream: return "H3_CLOSED_CRITICAL_STREAM";
        case h3_error_code::frame_unexpected: return "H3_FRAME_UNEXPECTED";
        case h3_error_code::frame_error: return "H3_FRAME_ERROR";
        case h3_error_code::excessive_load: return "H3_EXCESSIVE_LOAD";
        case h3_error_code::id_error: return "H3_ID_ERROR";
        case h3_error_code::settings_error: return "H3_SETTINGS_ERROR";
        case h3_error_code::missing_settings: return "H3_MISSING_SETTINGS";
        case h3_error_code::request_rejected: return "H3_REQUEST_REJECTED";
        case h3_error_code::request_cancelled: return "H3_REQUEST_CANCELLED";
        case h3_error_code::request_incomplete: return "H3_REQUEST_INCOMPLETE";
        case h3_error_code::message_error: return "H3_MESSAGE_ERROR";
        case h3_error_code::connect_error: return "H3_CONNECT_ERROR";
        case h3_error_code::version_fallback: return "H3_VERSION_FALLBACK";
        }
        return fmt::format("HTTP/3 error {:#x}", ev);
    }
};

} // anonymous namespace

const std::error_category& http3_error_category() noexcept {
    static const h3_error_category category;
    return category;
}

std::error_code make_error_code(h3_error_code e) noexcept {
    return {static_cast<int>(e), http3_error_category()};
}

} // namespace seastar::experimental::http3
