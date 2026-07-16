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
 * Copyright 2024 ScyllaDB
 */
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <system_error>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sstring.hh>

struct gnutls_certificate_credentials_st;

namespace seastar::net { class connected_socket_impl; }
namespace seastar::tls {
    class session_impl;
    class credentials_impl;
    class dh_params_impl;
    class dh_params;
    class certificate_credentials;
    enum class session_type;
    enum class x509_crt_format;
    enum class client_auth;
    enum class session_resume_mode;
    struct tls_options;
    typedef std::basic_string_view<char> blob;
}

namespace seastar::tls::gnutls {

/// Create a GnuTLS TLS session.
shared_ptr<session_impl> make_session(
    session_type type,
    shared_ptr<certificate_credentials> creds,
    std::unique_ptr<net::connected_socket_impl> sock,
    const tls_options& options);

/// Return the GnuTLS error category.
const std::error_category& error_category();

/// Generate a session ticket key using GnuTLS.
std::vector<uint8_t> generate_session_ticket_key();

/// Create a GnuTLS credentials implementation.
shared_ptr<credentials_impl> make_credentials_impl();

/// Create GnuTLS DH parameters from a security level.
std::unique_ptr<dh_params_impl> make_dh_params(dh_params::level);

/// Create GnuTLS DH parameters from raw data.
std::unique_ptr<dh_params_impl> make_dh_params(const blob&, x509_crt_format);

/// Initialize TLS error codes with GnuTLS values.
void init_error_codes();

/// A view of the GnuTLS-native state of a certificate_credentials, for the
/// QUIC crypto integration, which configures gnutls sessions directly
/// (handshake-level TLS) instead of going through the stream-oriented
/// session_impl.
///
/// The raw pointers and spans remain valid for as long as \c keepalive is
/// held.
struct quic_credentials_view {
    /// Keeps the backing credentials implementation (and thus \c xcred) alive.
    shared_ptr<credentials_impl> keepalive;
    /// The GnuTLS certificate credentials handle (gnutls_certificate_credentials_t).
    gnutls_certificate_credentials_st* xcred;
    client_auth cauth;
    session_resume_mode resume_mode;
    /// Server session ticket key material; empty unless resume_mode enables tickets.
    std::span<const uint8_t> session_resume_key;
    std::vector<sstring> alpn_protocols;
    bool enable_certificate_verification;
};

/// Extract the GnuTLS-native credential state needed to configure a QUIC
/// TLS session. Resolves once any lazily-requested system trust store has
/// been loaded into the credentials.
///
/// Throws if the credentials do not belong to the GnuTLS backend.
future<quic_credentials_view> get_quic_credentials_view(const certificate_credentials&);

} // namespace seastar::tls::gnutls
