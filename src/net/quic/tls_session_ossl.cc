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

#include "net/tls_openssl.hh"

#include <seastar/net/quic/error.hh>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <fmt/format.h>

#include <mutex>

namespace seastar::net::quic::internal {

namespace {

[[noreturn]] void throw_ossl_error(const char* what) {
    std::array<char, 256> buf;
    ERR_error_string_n(ERR_get_error(), buf.data(), buf.size());
    throw std::system_error(make_error_code(errc::crypto_error), fmt::format("{}: {}", what, buf.data()));
}

void ensure_ossl_initialized() {
    // ngtcp2_crypto_ossl_init() must run once per process before any other
    // ngtcp2_crypto_ossl function.
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (ngtcp2_crypto_ossl_init() != 0) {
            throw std::system_error(make_error_code(errc::crypto_error), "ngtcp2_crypto_ossl_init");
        }
    });
}

class ossl_tls_session final : public tls_session {
    shared_ptr<tls::certificate_credentials> _creds; // the SSL_CTX refers to credential-owned state
    tls::openssl::ssl_ctx_handle _ctx;
    ngtcp2_crypto_ossl_ctx* _ossl_ctx = nullptr;
    SSL* _ssl = nullptr;

public:
    ossl_tls_session(shared_ptr<tls::certificate_credentials> creds, bool server,
                     conn_ref_holder& holder, const client_tls_options* options)
        : _creds(std::move(creds)) {
        ensure_ossl_initialized();
        _ctx = tls::openssl::make_quic_ssl_context(*_creds,
                                                   server ? tls::session_type::SERVER : tls::session_type::CLIENT,
                                                   options ? options->alpn_protocols : std::vector<sstring>{});
        if (ngtcp2_crypto_ossl_ctx_new(&_ossl_ctx, nullptr) != 0) {
            throw std::system_error(make_error_code(errc::crypto_error), "ngtcp2_crypto_ossl_ctx_new");
        }
        try {
            configure(server, holder, options);
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ossl_tls_session(const ossl_tls_session&) = delete;

    ~ossl_tls_session() override {
        cleanup();
    }

    void* native_handle() noexcept override {
        return _ossl_ctx;
    }

    std::optional<sstring> selected_alpn() const override {
        const unsigned char* alpn = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(_ssl, &alpn, &len);
        if (alpn == nullptr) {
            return std::nullopt;
        }
        return sstring(reinterpret_cast<const char*>(alpn), len);
    }

    void handle_new_session(SSL_SESSION* session) {
        if (!_on_ticket) {
            return;
        }
        auto len = i2d_SSL_SESSION(session, nullptr);
        if (len <= 0) {
            return;
        }
        session_ticket ticket;
        ticket.data.resize(len);
        auto* p = ticket.data.data();
        i2d_SSL_SESSION(session, &p);
        _on_ticket(std::move(ticket));
    }

private:
    void cleanup() noexcept {
        if (_ssl) {
            // The ngtcp2_conn may not outlive the SSL object; clearing the
            // app data keeps the OpenSSL QUIC TLS callbacks from touching a
            // dangling conn reference during SSL_free (ossl backend rule).
            SSL_set_app_data(_ssl, nullptr);
            SSL_free(_ssl);
            _ssl = nullptr;
        }
        if (_ossl_ctx) {
            ngtcp2_crypto_ossl_ctx_del(_ossl_ctx);
            _ossl_ctx = nullptr;
        }
    }

    static int new_session_cb(SSL* ssl, SSL_SESSION* session) {
        auto* holder = static_cast<conn_ref_holder*>(SSL_get_app_data(ssl));
        if (holder != nullptr && holder->session != nullptr) {
            static_cast<ossl_tls_session*>(holder->session)->handle_new_session(session);
        }
        return 0; // no reference taken
    }

    void configure(bool server, conn_ref_holder& holder, const client_tls_options* options) {
        _ssl = SSL_new(_ctx.get());
        if (!_ssl) {
            throw_ossl_error("SSL_new");
        }
        ngtcp2_crypto_ossl_ctx_set_ssl(_ossl_ctx, _ssl);

        holder.session = this;
        if (server) {
            if (ngtcp2_crypto_ossl_configure_server_session(_ssl) != 0) {
                throw std::system_error(make_error_code(errc::crypto_error),
                                        "ngtcp2_crypto_ossl_configure_server_session");
            }
            SSL_set_app_data(_ssl, &holder);
            SSL_set_accept_state(_ssl);
        } else {
            if (ngtcp2_crypto_ossl_configure_client_session(_ssl) != 0) {
                throw std::system_error(make_error_code(errc::crypto_error),
                                        "ngtcp2_crypto_ossl_configure_client_session");
            }
            SSL_set_app_data(_ssl, &holder);
            SSL_set_connect_state(_ssl);
            if (!options->server_name.empty()) {
                SSL_set_tlsext_host_name(_ssl, options->server_name.c_str());
                if (!SSL_set1_host(_ssl, options->server_name.c_str())) {
                    throw_ossl_error("SSL_set1_host");
                }
            }
            // Capture session tickets for resumption.
            SSL_CTX_set_session_cache_mode(_ctx.get(), SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
            SSL_CTX_sess_set_new_cb(_ctx.get(), new_session_cb);
            if (options->ticket && !options->ticket->data.empty()) {
                const auto* p = options->ticket->data.data();
                if (auto* session = d2i_SSL_SESSION(nullptr, &p, options->ticket->data.size())) {
                    SSL_set_session(_ssl, session);
                    SSL_SESSION_free(session);
                }
            }
        }
    }
};

} // anonymous namespace

future<std::unique_ptr<tls_session>> make_ossl_server_session(
        shared_ptr<tls::server_credentials> creds, conn_ref_holder& holder) {
    return make_ready_future<std::unique_ptr<tls_session>>(
        std::make_unique<ossl_tls_session>(std::move(creds), true, holder, nullptr));
}

future<std::unique_ptr<tls_session>> make_ossl_client_session(
        shared_ptr<tls::certificate_credentials> creds, conn_ref_holder& holder, client_tls_options options) {
    return make_ready_future<std::unique_ptr<tls_session>>(
        std::make_unique<ossl_tls_session>(std::move(creds), false, holder, &options));
}

} // namespace seastar::net::quic::internal
