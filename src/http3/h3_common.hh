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

#include <seastar/http/reply.hh>
#include <seastar/http/request.hh>

#include <nghttp3/nghttp3.h>

#include <charconv>
#include <string_view>
#include <vector>

namespace seastar::experimental::http3::internal {

// Connection-specific / hop-by-hop headers that must not be forwarded on
// HTTP/3 (RFC 9114 §4.2).
inline constexpr bool is_forbidden_header(std::string_view name) noexcept {
    return name == "connection" || name == "keep-alive" || name == "proxy-connection"
        || name == "transfer-encoding" || name == "upgrade";
}

inline std::string_view to_view(const nghttp3_rcbuf* buf) {
    auto v = nghttp3_rcbuf_get_buf(const_cast<nghttp3_rcbuf*>(buf));
    return std::string_view(reinterpret_cast<const char*>(v.base), v.len);
}

inline sstring lowercase(std::string_view s) {
    sstring out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Apply a received request header (server side). Pseudo-headers map to
// request fields; the rest go into the header map (Host from :authority).
inline void apply_request_header(http::request& req, int32_t token,
                                 std::string_view name, std::string_view value) {
    switch (token) {
    case NGHTTP3_QPACK_TOKEN__METHOD:
        req._method = sstring(value);
        return;
    case NGHTTP3_QPACK_TOKEN__PATH:
        req._url = sstring(value);
        return;
    case NGHTTP3_QPACK_TOKEN__SCHEME:
        return; // scheme is implied
    case NGHTTP3_QPACK_TOKEN__AUTHORITY:
        req._headers["Host"] = sstring(value);
        return;
    default:
        break;
    }
    if (!name.empty() && name[0] == ':') {
        return; // unknown pseudo-header
    }
    req._headers[sstring(name)] = sstring(value);
}

// Apply a received response header (client side). :status parses the code.
inline void apply_reply_header(http::reply& rep, int32_t token,
                               std::string_view name, std::string_view value) {
    if (token == NGHTTP3_QPACK_TOKEN__STATUS) {
        int code = 0;
        std::from_chars(value.data(), value.data() + value.size(), code);
        rep._status = static_cast<http::reply::status_type>(code);
        return;
    }
    if (!name.empty() && name[0] == ':') {
        return;
    }
    rep._headers[sstring(name)] = sstring(value);
}

inline nghttp3_nv make_nv(std::string_view name, std::string_view value) {
    return nghttp3_nv{
        .name = reinterpret_cast<const uint8_t*>(name.data()),
        .value = reinterpret_cast<const uint8_t*>(value.data()),
        .namelen = name.size(),
        .valuelen = value.size(),
        .flags = NGHTTP3_NV_FLAG_NONE,
    };
}

// Build the response header list. \p storage owns the header-name/value
// strings the returned nv array points into, so it must outlive them.
inline std::vector<nghttp3_nv> reply_to_nva(const http::reply& rep, std::vector<sstring>& storage) {
    std::vector<nghttp3_nv> nva;
    storage.reserve(1 + rep._headers.size());
    storage.push_back(seastar::to_sstring(static_cast<int>(rep._status)));
    nva.push_back(make_nv(":status", storage.back()));
    for (const auto& [name, value] : rep._headers) {
        auto lname = lowercase(name);
        if (is_forbidden_header(lname)) {
            continue;
        }
        storage.push_back(std::move(lname));
        // The value can be referenced in place: the headers map outlives
        // the nv array (both live for the duration of the submit call).
        nva.push_back(make_nv(storage.back(), value));
    }
    return nva;
}

// Build the request header list (client side).
inline std::vector<nghttp3_nv> request_to_nva(const http::request& req, std::string_view authority,
                                              std::vector<sstring>& storage) {
    std::vector<nghttp3_nv> nva;
    storage.reserve(4 + req._headers.size());
    auto push = [&] (std::string_view n, std::string_view v) {
        storage.push_back(sstring(v));
        nva.push_back(make_nv(n, storage.back()));
    };
    push(":method", req._method.empty() ? std::string_view("GET") : std::string_view(req._method));
    push(":scheme", "https");
    push(":authority", authority);
    push(":path", req._url.empty() ? std::string_view("/") : std::string_view(req._url));
    for (const auto& [name, value] : req._headers) {
        auto lname = lowercase(name);
        if (is_forbidden_header(lname) || lname == "host") {
            continue;
        }
        storage.push_back(std::move(lname));
        // The value can be referenced in place: the headers map outlives
        // the nv array (both live for the duration of the submit call).
        nva.push_back(make_nv(storage.back(), value));
    }
    return nva;
}

} // namespace seastar::experimental::http3::internal
