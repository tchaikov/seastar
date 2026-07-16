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
#include <seastar/core/iostream.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/quic/error.hh>
#include <seastar/net/quic/types.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/tls.hh>

#include <chrono>
#include <expected>
#include <optional>

namespace seastar::net::quic {

/// \addtogroup quic-module
/// @{

namespace internal {
class quic_stream_impl;
class quic_connection_impl;
class server_impl;
}

/// \brief Transport parameters shared by client and server connections.
///
/// The defaults are reasonable for request/response workloads; bulk
/// transfer applications may want to raise the flow-control windows.
struct connection_config {
    /// Close the connection after this long without any activity
    /// (QUIC max_idle_timeout transport parameter).
    std::chrono::milliseconds max_idle_timeout{30'000};
    /// When non-zero, send keep-alive packets at this interval to fend
    /// off the idle timeout.
    std::chrono::milliseconds keep_alive_interval{0};
    /// Maximum number of concurrent peer-initiated bidirectional streams.
    uint64_t max_streams_bidi = 100;
    /// Maximum number of concurrent peer-initiated unidirectional streams.
    uint64_t max_streams_uni = 100;
    /// Connection-level flow control window.
    uint64_t initial_max_data = 1 << 20;
    /// Per-stream flow control window.
    uint64_t initial_max_stream_data = 256 << 10;
    /// Enable DATAGRAM frame support (RFC 9221).
    bool enable_datagrams = false;
    /// Disable UDP generic segmentation offload for this connection's
    /// sends (mainly a testing knob; GSO is also disabled automatically
    /// when the kernel rejects it).
    bool disable_gso = false;
    /// Local buffering budget per stream for written-but-unacknowledged
    /// data; writers stall once it is exhausted.
    size_t stream_send_buffer_size = 256 << 10;
};

/// \brief Options for \ref quic::connect().
struct connect_options {
    connection_config config{};
    /// TLS SNI and certificate-verification host name; defaults to the
    /// textual form of the remote address when empty.
    sstring server_name{};
    /// ALPN protocols in preference order. Required: QUIC application
    /// protocols must negotiate ALPN (e.g. "h3").
    std::vector<sstring> alpn_protocols{};
    /// TLS session ticket from a previous connection to the same server,
    /// enabling session resumption.
    std::optional<session_ticket> ticket{};
    /// Address-validation token from a NEW_TOKEN frame of a previous
    /// connection to the same server.
    std::optional<address_token> token{};
};

/// \brief Options for \ref quic::listen().
struct listen_options {
    connection_config config{};
    /// Force address validation: reply to the first Initial of every new
    /// connection with a Retry packet (QUIC Interop Runner "retry" case).
    bool require_retry = false;
    /// When set, write qlog (JSON-SEQ) traces for every connection into
    /// this directory, named after the original destination CID.
    std::optional<sstring> qlog_dir{};
    /// Depth of the queue of handshake-completed connections awaiting
    /// \ref server::accept().
    unsigned accept_queue_depth = 64;
};

/// \brief A QUIC stream: an ordered, reliable byte channel multiplexed
/// on a \ref connection (RFC 9000 §2).
///
/// Bidirectional streams carry data both ways; unidirectional streams
/// only from their initiator. The reading and writing sides are exposed
/// as regular Seastar \ref input_stream / \ref output_stream, so QUIC
/// streams plug into any stream-oriented consumer.
class stream {
    friend class internal::quic_connection_impl;
    lw_shared_ptr<internal::quic_stream_impl> _impl;
    explicit stream(lw_shared_ptr<internal::quic_stream_impl> impl) noexcept;
public:
    stream() noexcept;
    ~stream();
    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;

    /// \brief The QUIC stream identifier.
    stream_id id() const noexcept;

    /// \brief Whether the stream is bidirectional or unidirectional.
    stream_kind kind() const noexcept;

    /// \brief The readable side of the stream.
    ///
    /// Reading grants the peer flow-control credit as data is consumed.
    /// Must not be called on a locally-initiated unidirectional stream,
    /// and may be called at most once.
    input_stream<char> input();

    /// \brief The writable side of the stream.
    ///
    /// Closing the output stream sends a FIN, ending the stream
    /// gracefully. Must not be called on a peer-initiated unidirectional
    /// stream, and may be called at most once.
    output_stream<char> output();

    /// \brief Abruptly terminates the sending side (RESET_STREAM).
    ///
    /// Pending and future writes are discarded; the peer receives
    /// \p code as the reset reason.
    std::expected<void, std::error_code> reset_write(application_error_code code) noexcept;

    /// \brief Asks the peer to stop sending (STOP_SENDING).
    ///
    /// The peer is expected to reset its sending side with \p code; data
    /// already received can still be read.
    std::expected<void, std::error_code> stop_sending(application_error_code code) noexcept;
};

/// \brief Statistics of a \ref connection, from the transport layer.
struct connection_stats {
    /// Smoothed round-trip time estimate.
    std::chrono::nanoseconds smoothed_rtt;
    /// Congestion window, in bytes.
    uint64_t cwnd;
    /// Total bytes sent/received on the connection, including headers.
    uint64_t bytes_sent;
    uint64_t bytes_received;
    /// Datagrams that reached this connection via cross-shard forwarding.
    uint64_t forwarded_datagrams;
};

/// \brief A QUIC connection: a multiplexed, encrypted transport session
/// with a peer (RFC 9000).
///
/// Connections are obtained from \ref quic::connect() (client side) or
/// \ref server::accept() (server side); in both cases the TLS handshake
/// has already completed. A connection multiplexes many concurrent
/// streams, plus optional unreliable datagrams (RFC 9221).
class connection {
    friend class internal::quic_connection_impl;
    friend class internal::server_impl;
    lw_shared_ptr<internal::quic_connection_impl> _impl;
    explicit connection(lw_shared_ptr<internal::quic_connection_impl> impl) noexcept;
public:
    connection() noexcept;
    ~connection();
    connection(connection&&) noexcept;
    connection& operator=(connection&&) noexcept;

    /// \brief Opens a new locally-initiated stream.
    ///
    /// Resolves once stream credit is available (the peer bounds the
    /// number of concurrent streams).
    future<stream> open_stream(stream_kind kind);

    /// \brief Accepts the next peer-initiated stream of either kind
    /// (inspect \ref stream::kind()).
    future<stream> accept_stream();

    /// \brief Sends an unreliable DATAGRAM frame (RFC 9221).
    ///
    /// Requires \ref connection_config::enable_datagrams on both peers;
    /// fails with \ref errc::unsupported otherwise.
    future<> send_datagram(temporary_buffer<char> datagram);

    /// \brief Receives the next DATAGRAM frame from the peer.
    future<temporary_buffer<char>> receive_datagram();

    /// \brief Closes the connection with an application error code
    /// (CONNECTION_CLOSE, application space).
    ///
    /// Resolves once the closing exchange has finished or timed out.
    /// Idempotent.
    future<> close(application_error_code code = {}, sstring reason = {}) noexcept;

    /// \brief Resolves when the connection terminates for any reason
    /// (local or peer close, idle timeout, transport error). Never fails.
    future<> wait_closed() noexcept;

    /// \brief True once the connection terminated.
    bool is_closed() const noexcept;

    /// \brief The ALPN protocol negotiated during the handshake.
    std::optional<sstring> alpn() const;

    socket_address local_address() const;
    socket_address remote_address() const;

    /// \brief Migrates a client connection to a new local address
    /// (RFC 9000 §9).
    ///
    /// Binds a fresh UDP socket to \p new_local (port 0 picks an
    /// ephemeral port) and moves the connection onto the new path.
    /// With \p immediate the path is used right away
    /// (path validation still runs in the background); otherwise the
    /// returned future resolves after the new path has been validated.
    /// Only valid on client connections.
    future<> migrate(socket_address new_local, bool immediate = false);

    /// \brief Resolves when the server issues a TLS session ticket,
    /// yielding material for \ref connect_options::ticket. Client side.
    future<session_ticket> wait_for_session_ticket();

    /// \brief Takes the address-validation token received in a NEW_TOKEN
    /// frame, if any (for \ref connect_options::token). Client side.
    std::expected<address_token, std::error_code> take_address_token() noexcept;

    /// \brief Transport-level statistics.
    connection_stats get_stats() const noexcept;
};

/// \brief A per-shard QUIC server socket.
///
/// Bind one instance per shard to the same address (e.g. via
/// \ref seastar::sharded); the kernel distributes datagrams across the
/// shards' sockets (SO_REUSEPORT) and stray datagrams are forwarded to
/// the owning shard using the shard id embedded in server-generated
/// connection IDs.
class server {
    friend server listen(socket_address, shared_ptr<tls::server_credentials>, listen_options);
    lw_shared_ptr<internal::server_impl> _impl;
    explicit server(lw_shared_ptr<internal::server_impl> impl) noexcept;
public:
    server() noexcept;
    ~server();
    server(server&&) noexcept;
    server& operator=(server&&) noexcept;

    /// \brief Waits for the next incoming connection.
    ///
    /// The returned connection has already completed its handshake.
    future<connection> accept();

    /// \brief Makes pending and future accept() calls fail.
    void abort_accept();

    socket_address local_address() const;

    /// \brief Stops the server: closes all connections with \p code and
    /// releases the socket.
    future<> stop(application_error_code code = {}) noexcept;
};

/// \brief Starts listening for QUIC connections on \p addr.
///
/// Call on every shard with the same address. TLS certificates, ALPN
/// protocols (via \ref tls::server_credentials::set_alpn_protocols) and
/// client-authentication policy come from \p creds.
///
/// \note QUIC I/O uses kernel UDP sockets directly and is supported with
/// the default (posix) network stack only.
server listen(socket_address addr, shared_ptr<tls::server_credentials> creds, listen_options options = {});

/// \brief Establishes a QUIC connection to \p remote.
///
/// Resolves once the handshake has completed (the negotiated ALPN is
/// then available). \p creds supplies the trust store for certificate
/// verification.
future<connection> connect(socket_address remote, shared_ptr<tls::certificate_credentials> creds,
                           connect_options options);

/// @}

} // namespace seastar::net::quic
