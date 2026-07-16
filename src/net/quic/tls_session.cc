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

#include "tls_session.hh"

#include <fmt/format.h>

#include <string_view>

namespace seastar::net::quic::internal {

// Exactly one ngtcp2 crypto helper can be linked (their internal symbols
// collide), so the QUIC crypto provider is fixed at build time. The
// runtime provider (--crypto-provider) must match, because the
// tls credentials objects are created by the runtime provider and the
// QUIC session extracts provider-native handles from them.

namespace {

#ifdef SEASTAR_QUIC_GNUTLS
constexpr std::string_view quic_crypto_provider = "gnutls";
#else
constexpr std::string_view quic_crypto_provider = "openssl";
#endif

void check_crypto_provider() {
    std::string_view backend = tls::backend_name();
    if (backend != quic_crypto_provider) {
        throw std::runtime_error(fmt::format(
            "QUIC was built with the '{}' crypto provider, but the runtime provider is '{}'; "
            "run with --crypto-provider={} (or rebuild with -DSeastar_QUIC_CRYPTO_PROVIDER={})",
            quic_crypto_provider, backend, quic_crypto_provider, backend));
    }
}

} // anonymous namespace

future<std::unique_ptr<tls_session>> make_server_tls_session(
        shared_ptr<tls::server_credentials> creds, conn_ref_holder& holder) {
    check_crypto_provider();
#ifdef SEASTAR_QUIC_GNUTLS
    return make_gnutls_server_session(std::move(creds), holder);
#else
    return make_ossl_server_session(std::move(creds), holder);
#endif
}

future<std::unique_ptr<tls_session>> make_client_tls_session(
        shared_ptr<tls::certificate_credentials> creds, conn_ref_holder& holder, client_tls_options options) {
    check_crypto_provider();
#ifdef SEASTAR_QUIC_GNUTLS
    return make_gnutls_client_session(std::move(creds), holder, std::move(options));
#else
    return make_ossl_client_session(std::move(creds), holder, std::move(options));
#endif
}

} // namespace seastar::net::quic::internal
