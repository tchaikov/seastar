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

#include <seastar/core/abort_source.hh>
#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/request.hh>
#include <seastar/net/quic/quic.hh>
#include <seastar/util/noncopyable_function.hh>

#include <optional>

namespace seastar::experimental::http3 {

/// \addtogroup http3-module
/// @{

namespace internal {
class client_connection;
}

/// \brief An HTTP/3 client that multiplexes requests over one QUIC
/// connection.
///
/// The API mirrors \ref seastar::http::client::make_request(). Unlike the
/// TCP client there is no connection pool: concurrent requests become
/// concurrent bidirectional streams on a single, lazily-established QUIC
/// connection.
class client {
public:
    /// Called with the response and a stream over its body.
    using reply_handler = noncopyable_function<future<>(const http::reply&, input_stream<char>&& body)>;

    /// \brief Creates a client for \p addr.
    /// \param creds TLS trust store for certificate verification
    /// \param host value for the :authority pseudo-header and TLS SNI
    client(socket_address addr, shared_ptr<tls::certificate_credentials> creds, sstring host);
    ~client();
    client(client&&) noexcept;
    client& operator=(client&&) noexcept;

    /// \brief Sends \p req and invokes \p handle with the response.
    ///
    /// Opens a new bidirectional stream; concurrent calls multiplex on the
    /// connection. When \p expected is set and the response status differs,
    /// the returned future fails.
    ///
    /// When \p as is given and aborts, the request stream is cancelled
    /// (H3_REQUEST_CANCELLED is signalled to the server) and the returned
    /// future fails; a body stream being read by \p handle fails as well.
    future<> make_request(http::request req, reply_handler handle,
                          std::optional<http::reply::status_type> expected = std::nullopt,
                          abort_source* as = nullptr);

    /// \brief Closes the connection and waits for background work.
    future<> close() noexcept;

private:
    future<> ensure_connected();

    socket_address _addr;
    shared_ptr<tls::certificate_credentials> _creds;
    sstring _host;
    shared_ptr<internal::client_connection> _conn;
    std::optional<shared_future<>> _connecting;
};

/// @}

} // namespace seastar::experimental::http3
