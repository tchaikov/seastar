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
#include <seastar/core/gate.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sharded.hh>
#include <seastar/http/routes.hh>
#include <seastar/http3/error.hh>
#include <seastar/net/quic/quic.hh>

#include <functional>

namespace seastar::experimental::http3 {

/// \addtogroup http3-module
/// @{

namespace internal {
class server_connection;
}

/// \brief An HTTP/3 server (RFC 9114) over the experimental QUIC transport.
///
/// Reuses \ref seastar::httpd::routes and \ref seastar::http::request /
/// \ref seastar::http::reply, so route handlers written for
/// \ref seastar::httpd::http_server work unchanged over HTTP/3. One
/// instance serves one shard; use \ref http3_server_control to run a
/// sharded server.
class http3_server {
public:
    /// The route table; populate it before calling \ref listen().
    httpd::routes _routes;

    http3_server() = default;
    ~http3_server();

    /// \brief Starts listening for HTTP/3 connections on \p addr.
    ///
    /// The "h3" ALPN protocol is added to \p creds automatically.
    /// \param addr UDP address (per-shard; the transport handles
    ///        SO_REUSEPORT and CID routing)
    future<> listen(socket_address addr, shared_ptr<tls::server_credentials> creds,
                    net::quic::listen_options options = {});

    /// \brief Graceful shutdown: GOAWAY all connections and close them.
    future<> stop();

    /// \brief Whether request bodies are streamed to handlers.
    ///
    /// When enabled, handlers read the body from
    /// \ref http::request::content_stream (which grants QUIC flow-control
    /// credit as it is consumed, backpressuring the client); when disabled
    /// (the default, matching \ref httpd::http_server), the whole body is
    /// buffered into \ref http::request::content before dispatch.
    void set_content_streaming(bool b) noexcept {
        _content_streaming = b;
    }
    /// \brief Whether request bodies are streamed to handlers (see
    /// \ref set_content_streaming()).
    bool get_content_streaming() const noexcept {
        return _content_streaming;
    }

    /// \brief The bound local address (valid after \ref listen()).
    socket_address local_address() const;

    /// \brief Total number of requests dispatched so far.
    uint64_t requests_served() const noexcept {
        return _requests_served;
    }

private:
    friend class internal::server_connection;
    future<> accept_loop();

    std::optional<net::quic::server> _server;
    gate _gate;
    std::vector<shared_ptr<internal::server_connection>> _connections;
    bool _content_streaming = false;
    uint64_t _requests_served = 0;
    bool _stopping = false;
};

/// \brief Sharded lifecycle wrapper for \ref http3_server, mirroring
/// \ref seastar::httpd::http_server_control.
class http3_server_control {
public:
    http3_server_control();

    /// \brief Starts the sharded server.
    future<> start();
    /// \brief Stops the sharded server.
    future<> stop() noexcept;
    /// \brief Installs routes on every shard.
    future<> set_routes(std::function<void(httpd::routes&)> fun);
    /// \brief Listens on \p addr on every shard.
    ///
    /// Every shard binds the same address (SO_REUSEPORT); the kernel
    /// spreads datagrams across the shards' sockets and stray datagrams
    /// are forwarded to the owning shard via the shard id embedded in
    /// server connection IDs. \p builder is used to build each shard its
    /// own credentials (a \ref tls::server_credentials object must not be
    /// shared across shards).
    future<> listen(socket_address addr, tls::credentials_builder builder,
                    net::quic::listen_options options = {});

    /// \brief Access the underlying sharded server.
    sharded<http3_server>& server() {
        return *_server;
    }

private:
    std::unique_ptr<sharded<http3_server>> _server;
};

/// @}

} // namespace seastar::experimental::http3
