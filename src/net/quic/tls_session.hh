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

#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sstring.hh>
#include <seastar/net/quic/types.hh>
#include <seastar/net/tls.hh>
#include <seastar/util/noncopyable_function.hh>

#include <ngtcp2/ngtcp2_crypto.h>

#include <memory>
#include <optional>

namespace seastar::net::quic::internal {

class tls_session;

/// The object whose address is installed as the TLS library's per-session
/// user pointer (gnutls_session_set_ptr / SSL_set_app_data). The ngtcp2
/// crypto helpers require that pointer to be a ngtcp2_crypto_conn_ref, so
/// it must be the first member; our own hooks use it to reach the owning
/// tls_session as well.
struct conn_ref_holder {
    ngtcp2_crypto_conn_ref conn_ref;
    tls_session* session = nullptr;
};
static_assert(std::is_standard_layout_v<conn_ref_holder>);

/// Client-side TLS parameters for a QUIC connection.
struct client_tls_options {
    /// SNI and certificate-verification host name; may be empty.
    sstring server_name;
    /// ALPN protocols, in preference order; must not be empty (QUIC
    /// application protocols require ALPN).
    std::vector<sstring> alpn_protocols;
    /// TLS session to resume, previously captured via the ticket callback.
    std::optional<session_ticket> ticket;
};

/// A backend-native TLS 1.3 handshake context for one QUIC connection.
///
/// Unlike tls::session_impl, which runs TLS over a byte stream, this
/// wraps the handshake-level API that ngtcp2's crypto helpers drive: the
/// embedder feeds CRYPTO frames and the TLS library emits handshake data
/// and traffic secrets directly into the ngtcp2_conn.
class tls_session {
public:
    virtual ~tls_session() = default;

    /// The pointer to install with ngtcp2_conn_set_tls_native_handle():
    /// a gnutls_session_t or a ngtcp2_crypto_ossl_ctx*.
    virtual void* native_handle() noexcept = 0;

    /// The ALPN protocol negotiated during the handshake, if any.
    virtual std::optional<sstring> selected_alpn() const = 0;

    /// Invoked by the connection when the QUIC handshake completes.
    /// Backends use it to release retained handshake data or to send
    /// session tickets (servers with session resumption enabled).
    virtual void on_handshake_completed() {}

    /// Registers a callback invoked when the peer issues a TLS session
    /// ticket (client side); the captured ticket can be used for session
    /// resumption via client_tls_options::ticket.
    void set_ticket_callback(noncopyable_function<void(session_ticket)> cb) {
        _on_ticket = std::move(cb);
    }

protected:
    noncopyable_function<void(session_ticket)> _on_ticket;
};

#ifdef SEASTAR_QUIC_GNUTLS
future<std::unique_ptr<tls_session>> make_gnutls_server_session(
    shared_ptr<tls::server_credentials> creds, conn_ref_holder& holder);
future<std::unique_ptr<tls_session>> make_gnutls_client_session(
    shared_ptr<tls::certificate_credentials> creds, conn_ref_holder& holder, client_tls_options options);
#endif

#ifdef SEASTAR_QUIC_OSSL
future<std::unique_ptr<tls_session>> make_ossl_server_session(
    shared_ptr<tls::server_credentials> creds, conn_ref_holder& holder);
future<std::unique_ptr<tls_session>> make_ossl_client_session(
    shared_ptr<tls::certificate_credentials> creds, conn_ref_holder& holder, client_tls_options options);
#endif

/// Creates a server-side TLS session with the build-time selected QUIC
/// crypto provider. The runtime crypto provider (tls::backend_name())
/// must match it, since the credentials are provider-specific.
future<std::unique_ptr<tls_session>> make_server_tls_session(
    shared_ptr<tls::server_credentials> creds, conn_ref_holder& holder);

/// Creates a client-side TLS session with the build-time selected QUIC
/// crypto provider. The runtime crypto provider (tls::backend_name())
/// must match it, since the credentials are provider-specific.
future<std::unique_ptr<tls_session>> make_client_tls_session(
    shared_ptr<tls::certificate_credentials> creds, conn_ref_holder& holder, client_tls_options options);

} // namespace seastar::net::quic::internal
