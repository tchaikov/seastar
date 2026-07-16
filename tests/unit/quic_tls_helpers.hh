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

#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/net/tls.hh>

#include <boost/dll/runtime_symbol_info.hpp>

// TLS credentials over the build-generated test certificates, shared by the
// QUIC and HTTP/3 tests.
namespace quic_test_helpers {

inline std::string certfile(const std::string& file) {
    static const auto cert_location = boost::dll::program_location().parent_path();
    return (cert_location / file).string();
}

// Server credentials from the shared test certificate, advertising the
// given ALPN protocols (HTTP/3 sets its ALPN itself; pass none there).
inline seastar::future<seastar::shared_ptr<seastar::tls::server_credentials>>
make_server_creds(std::vector<seastar::sstring> alpn = {}) {
    auto creds = seastar::make_shared<seastar::tls::server_credentials>();
    co_await creds->set_x509_key_file(certfile("test.crt"), certfile("test.key"),
                                      seastar::tls::x509_crt_format::PEM);
    if (!alpn.empty()) {
        creds->set_alpn_protocols(std::move(alpn));
    }
    co_return creds;
}

inline seastar::future<seastar::shared_ptr<seastar::tls::certificate_credentials>>
make_client_creds() {
    auto creds = seastar::make_shared<seastar::tls::certificate_credentials>();
    co_await creds->set_x509_trust_file(certfile("catest.pem"), seastar::tls::x509_crt_format::PEM);
    co_return creds;
}

} // namespace quic_test_helpers
