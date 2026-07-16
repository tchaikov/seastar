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

#include <seastar/core/condition-variable.hh>
#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/quic/types.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/util/noncopyable_function.hh>

#include <ngtcp2/ngtcp2.h>

#include <deque>
#include <expected>
#include <optional>

namespace seastar::net {
class connected_socket_impl;
}

namespace seastar::net::quic::internal {

class quic_connection_impl;

/// State of one QUIC stream, shared between the public stream handle,
/// its input/output streams and the owning connection.
///
/// Receive path: the connection's ngtcp2 recv_stream_data callback
/// appends chunks; consumers pop them. Flow-control credit is granted on
/// consumption (not on receipt), which backpressures the peer.
///
/// Send path: writers append chunks bounded by a local buffering budget;
/// the connection fiber feeds them to ngtcp2_conn_writev_stream. Bytes
/// must stay available until acknowledged (ngtcp2 re-reads them on
/// retransmission), so chunks are only released by on_acked().
class quic_stream_impl : public enable_lw_shared_from_this<quic_stream_impl> {
public:
    /// One chunk delivered by the raw receive API.
    struct recv_chunk {
        temporary_buffer<char> data;
        bool fin = false;
    };

    quic_stream_impl(lw_shared_ptr<quic_connection_impl> conn, stream_id id, stream_kind kind, size_t send_buffer_size);

    stream_id id() const noexcept {
        return _id;
    }
    stream_kind kind() const noexcept {
        return _kind;
    }
    bool is_locally_initiated() const noexcept;

    // Consumer-facing receive side.

    /// Next chunk of stream data; empty data + fin=false means the read
    /// failed (reset). Does NOT grant flow-control credit; pair with
    /// consume().
    future<recv_chunk> receive();
    /// Grants n bytes of stream + connection flow-control credit.
    void consume(size_t n) noexcept;
    /// data_source-style read: receive() + consume() in one step.
    future<temporary_buffer<char>> read_some();

    input_stream<char> input();
    output_stream<char> output();
    /// The raw data_source/data_sink the iostreams above wrap; subject to
    /// the same one-shot and stream-direction rules.
    data_source make_source();
    data_sink make_sink();
    /// Resolves once the peer's sending side ends (FIN or reset) or the
    /// stream/connection goes away; the connected_socket adapter's
    /// wait_input_shutdown().
    future<> wait_input_shutdown();
    /// The owning connection's addresses.
    socket_address local_address() const noexcept;
    socket_address remote_address() const noexcept;

    // Consumer-facing send side.

    /// Queues data for sending, respecting the local buffering budget.
    future<> write(temporary_buffer<char> data);
    /// Marks the sending side finished (FIN); resolves when the FIN has
    /// been handed to the transport.
    future<> close_write();
    /// Zero-copy send: queues the given spans by reference (no copy),
    /// retained for retransmission until acknowledged. \p keepalive owns
    /// the memory the spans reference and is released only once every byte
    /// of this batch has been acknowledged (mirroring the posix TCP
    /// data_sink's deleter), so the referenced buffers need no external
    /// lifetime guarantee. Backpressure is the caller's responsibility
    /// (there is no local send budget on this path).
    void send_borrowed(std::span<const std::span<const char>> data, bool fin, deleter keepalive);
    /// Registers a callback invoked (on this shard) with the number of
    /// newly-acknowledged send bytes, so a zero-copy producer can release
    /// the underlying buffers. Used by the HTTP/3 layer.
    void set_ack_notifier(noncopyable_function<void(size_t)> fn) {
        _ack_notifier = std::move(fn);
    }
    std::expected<void, std::error_code> reset_write(application_error_code code) noexcept;
    std::expected<void, std::error_code> stop_sending(application_error_code code) noexcept;

    // Connection-facing hooks (ngtcp2 callbacks).

    void on_data(std::span<const uint8_t> data, bool fin);
    void on_reset(uint64_t app_error_code) noexcept;      // peer RESET_STREAM
    void on_stop_sending(uint64_t app_error_code) noexcept; // peer STOP_SENDING
    void on_acked(uint64_t offset) noexcept;              // cumulative ack
    void on_extend_max_stream_data() noexcept;            // window opened
    void on_closed(std::optional<uint64_t> app_error_code) noexcept; // stream gone
    void on_connection_error(std::exception_ptr ep) noexcept;

    // Connection-facing send-queue access.

    /// True if the stream has queued data or a pending FIN to hand to
    /// the transport.
    bool wants_send() const noexcept;
    /// Vectors over the not-yet-submitted portion of the send queue.
    size_t pending_vectors(std::span<ngtcp2_vec> out) const noexcept;
    bool fin_pending() const noexcept {
        return _fin_requested && !_fin_submitted;
    }
    /// Records that ngtcp2 accepted n more bytes (and possibly the FIN).
    void advance_submitted(size_t n, bool fin_included) noexcept;
    /// Transport-blocked bookkeeping (STREAM_DATA_BLOCKED and friends).
    void set_blocked(bool blocked) noexcept {
        _blocked = blocked;
    }
    /// Membership in the connection's send-ready queue, maintained by the
    /// connection so notify_sendable() avoids scanning the queue.
    bool in_send_ready() const noexcept {
        return _in_send_ready;
    }
    void set_in_send_ready(bool v) noexcept {
        _in_send_ready = v;
    }
    bool blocked() const noexcept {
        return _blocked;
    }

private:
    void release_send_bytes(size_t n) noexcept;
    std::expected<void, std::error_code> check_writable() const noexcept;

    lw_shared_ptr<quic_connection_impl> _conn;
    stream_id _id;
    stream_kind _kind;

    // receive side
    std::deque<temporary_buffer<char>> _recv_q;
    condition_variable _recv_cv;
    bool _recv_fin = false;
    std::optional<uint64_t> _recv_reset_error;
    std::exception_ptr _conn_error;

    // A queued send chunk: either owns its bytes (iostream path,
    // charged against the send budget) or borrows them (zero-copy path).
    // A borrowed chunk carries no keepalive of its own; the last chunk of
    // each send_borrowed() batch holds a single `keepalive` deleter that
    // owns the memory every span in the batch references, released only
    // once the whole batch has been acknowledged.
    struct out_chunk {
        std::span<const char> view;         // the bytes to send
        temporary_buffer<char> owned;       // holds `view` when owned
        bool is_owned;
        deleter keepalive;                  // set on the last borrowed chunk
        size_t size() const noexcept {
            return view.size();
        }
    };

    // send side
    std::deque<out_chunk> _send_q;  // retained until acked
    size_t _send_q_bytes = 0;       // total bytes currently in _send_q
    size_t _send_q_submitted = 0;   // bytes of _send_q already given to ngtcp2
    uint64_t _acked_offset = 0;     // cumulative acked stream offset
    semaphore _send_budget;
    size_t _send_budget_size;
    noncopyable_function<void(size_t)> _ack_notifier; // zero-copy ack callback
    std::optional<promise<>> _fin_flushed;
    bool _fin_requested = false;
    bool _fin_submitted = false;
    bool _write_side_reset = false; // local reset_write or peer STOP_SENDING
    bool _blocked = false;
    bool _in_send_ready = false;
    bool _closed = false;

    bool _input_taken = false;
    bool _output_taken = false;
};

/// Wraps a (bidirectional) stream impl as a net::connected_socket_impl, so
/// a QUIC stream can be used anywhere a connected_socket is expected.
std::unique_ptr<net::connected_socket_impl> make_connected_socket_impl(lw_shared_ptr<quic_stream_impl> stream);

} // namespace seastar::net::quic::internal
