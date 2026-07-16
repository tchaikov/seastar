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

#include "net/tls_gnutls.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/net/quic/error.hh>

#include <gnutls/gnutls.h>

#include <ngtcp2/ngtcp2_crypto_gnutls.h>

namespace seastar::net::quic::internal {

namespace {

// QUIC mandates TLS 1.3 (RFC 9001 §4.2); TLS 1.3 middlebox compatibility
// mode must not be used (§8.4).
constexpr const char* quic_priority = "%DISABLE_TLS13_COMPAT_MODE:NORMAL:-VERS-ALL:+VERS-TLS1.3";

void gtls_chk(int rv, const char* what) {
    if (rv != 0) {
        throw std::system_error(rv, tls::gnutls::error_category(), what);
    }
}

class gnutls_tls_session final : public tls_session {
    tls::gnutls::quic_credentials_view _creds;
    gnutls_session_t _session = nullptr;
    // gnutls_session_set_verify_cert() stores this hostname pointer and
    // dereferences it during the handshake, so the session must own it.
    sstring _verify_hostname;
    bool _server;
    bool _send_ticket_on_completion = false;

public:
    gnutls_tls_session(tls::gnutls::quic_credentials_view creds, bool server,
                       conn_ref_holder& holder, const client_tls_options* options)
        : _creds(std::move(creds))
        , _server(server) {
        unsigned flags = GNUTLS_ENABLE_EARLY_DATA | GNUTLS_NO_END_OF_EARLY_DATA;
        flags |= server ? (GNUTLS_SERVER | GNUTLS_NO_AUTO_SEND_TICKET) : GNUTLS_CLIENT;
        gtls_chk(gnutls_init(&_session, flags), "gnutls_init");
        try {
            configure(holder, options);
        } catch (...) {
            gnutls_deinit(_session);
            throw;
        }
    }

    gnutls_tls_session(const gnutls_tls_session&) = delete;

    ~gnutls_tls_session() override {
        gnutls_deinit(_session);
    }

    void* native_handle() noexcept override {
        return _session;
    }

    std::optional<sstring> selected_alpn() const override {
        gnutls_datum_t alpn;
        if (gnutls_alpn_get_selected_protocol(_session, &alpn) != 0) {
            return std::nullopt;
        }
        return sstring(reinterpret_cast<const char*>(alpn.data), alpn.size);
    }

    void on_handshake_completed() override {
        if (_send_ticket_on_completion) {
            // With GNUTLS_NO_AUTO_SEND_TICKET the server issues session
            // tickets explicitly, once the handshake is confirmed.
            gnutls_session_ticket_send(_session, 1, 0);
        }
    }

    void handle_new_ticket() {
        if (!_on_ticket) {
            return;
        }
        gnutls_datum_t data;
        if (gnutls_session_get_data2(_session, &data) != 0) {
            return;
        }
        session_ticket ticket;
        ticket.data.assign(data.data, data.data + data.size);
        gnutls_free(data.data);
        _on_ticket(std::move(ticket));
    }

private:
    static int new_ticket_hook(gnutls_session_t session, unsigned htype, unsigned, unsigned, const gnutls_datum_t*) {
        if (htype == GNUTLS_HANDSHAKE_NEW_SESSION_TICKET) {
            auto* holder = static_cast<conn_ref_holder*>(gnutls_session_get_ptr(session));
            static_cast<gnutls_tls_session*>(holder->session)->handle_new_ticket();
        }
        return 0;
    }

    void configure(conn_ref_holder& holder, const client_tls_options* options) {
        gtls_chk(gnutls_priority_set_direct(_session, quic_priority, nullptr), "gnutls_priority_set_direct");
        if (_server) {
            if (ngtcp2_crypto_gnutls_configure_server_session(_session) != 0) {
                throw std::system_error(make_error_code(errc::crypto_error),
                                        "ngtcp2_crypto_gnutls_configure_server_session");
            }
        } else {
            gnutls_handshake_set_hook_function(_session, GNUTLS_HANDSHAKE_NEW_SESSION_TICKET,
                                               GNUTLS_HOOK_POST, new_ticket_hook);
            if (ngtcp2_crypto_gnutls_configure_client_session(_session) != 0) {
                throw std::system_error(make_error_code(errc::crypto_error),
                                        "ngtcp2_crypto_gnutls_configure_client_session");
            }
        }
        gnutls_session_set_ptr(_session, &holder);
        holder.session = this;

        gtls_chk(gnutls_credentials_set(_session, GNUTLS_CRD_CERTIFICATE, _creds.xcred),
                 "gnutls_credentials_set");

        const std::vector<sstring>* alpn = nullptr;
        if (_server) {
            switch (_creds.cauth) {
            case tls::client_auth::NONE:
                break;
            case tls::client_auth::REQUEST:
                gnutls_certificate_server_set_request(_session, GNUTLS_CERT_REQUEST);
                break;
            case tls::client_auth::REQUIRE:
                gnutls_certificate_server_set_request(_session, GNUTLS_CERT_REQUIRE);
                break;
            }
            if (_creds.resume_mode != tls::session_resume_mode::NONE && !_creds.session_resume_key.empty()) {
                gnutls_datum_t key{const_cast<uint8_t*>(_creds.session_resume_key.data()),
                                   static_cast<unsigned>(_creds.session_resume_key.size())};
                gtls_chk(gnutls_session_ticket_enable_server(_session, &key),
                         "gnutls_session_ticket_enable_server");
                _send_ticket_on_completion = true;
            }
            alpn = &_creds.alpn_protocols;
        } else {
            if (!options->server_name.empty()) {
                gtls_chk(gnutls_server_name_set(_session, GNUTLS_NAME_DNS,
                                                options->server_name.c_str(), options->server_name.size()),
                         "gnutls_server_name_set");
            }
            if (_creds.enable_certificate_verification) {
                _verify_hostname = options->server_name;
                gnutls_session_set_verify_cert(_session,
                                               _verify_hostname.empty() ? nullptr : _verify_hostname.c_str(),
                                               0);
            }
            if (options->ticket && !options->ticket->data.empty()) {
                gtls_chk(gnutls_session_set_data(_session, options->ticket->data.data(), options->ticket->data.size()),
                         "gnutls_session_set_data");
            }
            alpn = &options->alpn_protocols;
        }

        if (!alpn->empty()) {
            std::vector<gnutls_datum_t> protos;
            protos.reserve(alpn->size());
            for (const auto& p : *alpn) {
                protos.push_back({const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(p.data())),
                                  static_cast<unsigned>(p.size())});
            }
            unsigned flags = GNUTLS_ALPN_MANDATORY;
            if (_server) {
                flags |= GNUTLS_ALPN_SERVER_PRECEDENCE;
            }
            gtls_chk(gnutls_alpn_set_protocols(_session, protos.data(), protos.size(), flags),
                     "gnutls_alpn_set_protocols");
        }
    }
};

} // anonymous namespace

future<std::unique_ptr<tls_session>> make_gnutls_server_session(
        shared_ptr<tls::server_credentials> creds, conn_ref_holder& holder) {
    auto view = co_await tls::gnutls::get_quic_credentials_view(*creds);
    co_return std::make_unique<gnutls_tls_session>(std::move(view), true, holder, nullptr);
}

future<std::unique_ptr<tls_session>> make_gnutls_client_session(
        shared_ptr<tls::certificate_credentials> creds, conn_ref_holder& holder, client_tls_options options) {
    auto view = co_await tls::gnutls::get_quic_credentials_view(*creds);
    co_return std::make_unique<gnutls_tls_session>(std::move(view), false, holder, &options);
}

} // namespace seastar::net::quic::internal
