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

#include "stream.hh"
#include "connection.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/net/api.hh>
#include <seastar/net/quic/error.hh>
#include <seastar/net/stack.hh>

namespace seastar::net::quic::internal {

quic_stream_impl::quic_stream_impl(lw_shared_ptr<quic_connection_impl> conn, stream_id id, stream_kind kind,
                                   size_t send_buffer_size)
    : _conn(std::move(conn))
    , _id(id)
    , _kind(kind)
    , _send_budget(send_buffer_size)
    , _send_budget_size(send_buffer_size) {
}

bool quic_stream_impl::is_locally_initiated() const noexcept {
    // Bit 0 of a stream id names the initiator: 0 = client, 1 = server.
    return (_id & 0x1) == (_conn->is_server() ? 1 : 0);
}

// Receive side.

future<quic_stream_impl::recv_chunk> quic_stream_impl::receive() {
    co_await _recv_cv.when([this] {
        return !_recv_q.empty() || _recv_fin || _recv_reset_error.has_value() || _conn_error || _closed;
    });
    if (_recv_reset_error) {
        throw connection_error(*_recv_reset_error, true, "stream reset by peer");
    }
    if (!_recv_q.empty()) {
        auto data = std::move(_recv_q.front());
        _recv_q.pop_front();
        bool fin = _recv_fin && _recv_q.empty();
        co_return recv_chunk{std::move(data), fin};
    }
    if (_recv_fin) {
        co_return recv_chunk{{}, true};
    }
    // Stream or connection went away without a FIN.
    std::rethrow_exception(_conn_error ? _conn_error : _conn->make_error());
}

void quic_stream_impl::consume(size_t n) noexcept {
    if (n > 0) {
        _conn->extend_credit(_id, n);
    }
}

future<temporary_buffer<char>> quic_stream_impl::read_some() {
    auto chunk = co_await receive();
    consume(chunk.data.size());
    co_return std::move(chunk.data);
}

// Send side.

std::expected<void, std::error_code> quic_stream_impl::check_writable() const noexcept {
    if (_conn_error || _closed) {
        return std::unexpected(make_error_code(errc::closing));
    }
    if (_write_side_reset) {
        return std::unexpected(make_error_code(errc::stream_state));
    }
    if (_fin_requested) {
        return std::unexpected(make_error_code(errc::stream_state));
    }
    return {};
}

future<> quic_stream_impl::write(temporary_buffer<char> data) {
    while (!data.empty()) {
        if (auto w = check_writable(); !w) {
            throw std::system_error(w.error(), "QUIC stream not writable");
        }
        // Split writes larger than the budget so they can ever complete.
        auto len = std::min(data.size(), _send_budget_size);
        co_await _send_budget.wait(len);
        if (auto w = check_writable(); !w) {
            _send_budget.signal(len);
            throw std::system_error(w.error(), "QUIC stream not writable");
        }
        auto chunk = data.share(0, len);
        data.trim_front(len);
        _send_q_bytes += chunk.size();
        std::span<const char> view(chunk.get(), chunk.size());
        _send_q.push_back(out_chunk{.view = view, .owned = std::move(chunk), .is_owned = true});
        _conn->notify_sendable(*this);
    }
}

future<> quic_stream_impl::close_write() {
    if (auto w = check_writable(); !w) {
        throw std::system_error(w.error(), "QUIC stream not writable");
    }
    _fin_requested = true;
    _fin_flushed.emplace();
    auto f = _fin_flushed->get_future();
    _conn->notify_sendable(*this);
    return f;
}

void quic_stream_impl::send_borrowed(std::span<const std::span<const char>> data, bool fin, deleter keepalive) {
    if (!check_writable()) {
        return;
    }
    // No copy: queue the spans by reference. The keepalive owns the memory
    // they point into and is attached to the last queued chunk, so it is
    // dropped (freeing the memory) only once the whole batch is acked.
    out_chunk* last = nullptr;
    for (const auto& span : data) {
        if (span.empty()) {
            continue;
        }
        _send_q_bytes += span.size();
        _send_q.push_back(out_chunk{.view = span, .is_owned = false});
        last = &_send_q.back();
    }
    if (last) {
        last->keepalive = std::move(keepalive);
    }
    if (fin) {
        _fin_requested = true;
    }
    _conn->notify_sendable(*this);
}

std::expected<void, std::error_code> quic_stream_impl::reset_write(application_error_code code) noexcept {
    if (_write_side_reset || _closed) {
        return std::unexpected(make_error_code(errc::stream_state));
    }
    auto r = _conn->shutdown_stream_write(_id, code.value);
    if (!r) {
        return r;
    }
    _write_side_reset = true;
    if (_fin_flushed) {
        _fin_flushed->set_exception(std::make_exception_ptr(
            std::system_error(make_error_code(errc::stream_state), "stream reset")));
        _fin_flushed.reset();
    }
    return {};
}

std::expected<void, std::error_code> quic_stream_impl::stop_sending(application_error_code code) noexcept {
    if (_closed) {
        return std::unexpected(make_error_code(errc::stream_state));
    }
    return _conn->shutdown_stream_read(_id, code.value);
}

// Connection-facing hooks.

void quic_stream_impl::on_data(std::span<const uint8_t> data, bool fin) {
    if (_recv_reset_error || _closed) {
        return;
    }
    if (!data.empty()) {
        _recv_q.emplace_back(reinterpret_cast<const char*>(data.data()), data.size());
    }
    if (fin) {
        _recv_fin = true;
    }
    _recv_cv.broadcast();
}

void quic_stream_impl::on_reset(uint64_t app_error_code) noexcept {
    // RESET_STREAM discards anything not yet delivered.
    _recv_reset_error = app_error_code;
    _recv_q.clear();
    _recv_cv.broadcast();
}

void quic_stream_impl::on_stop_sending(uint64_t app_error_code) noexcept {
    // The peer is no longer interested; ngtcp2 resets our sending side.
    _write_side_reset = true;
    if (_fin_flushed) {
        _fin_flushed->set_exception(std::make_exception_ptr(connection_error(
            app_error_code, true, "peer stopped reading")));
        _fin_flushed.reset();
    }
}

void quic_stream_impl::on_acked(uint64_t offset) noexcept {
    // Release fully-acknowledged chunks (they were retained for
    // retransmission).
    size_t borrowed_acked = 0;
    size_t owned_acked = 0;
    while (!_send_q.empty()) {
        auto& front = _send_q.front();
        auto chunk_size = front.size();
        if (_acked_offset + chunk_size > offset) {
            break;
        }
        bool owned = front.is_owned;
        _acked_offset += chunk_size;
        _send_q_bytes -= chunk_size;
        _send_q_submitted -= chunk_size;
        // pop_front() drops any keepalive on this chunk, freeing the
        // borrowed memory it kept alive for retransmission.
        _send_q.pop_front();
        if (owned) {
            // The iostream path charges the send budget.
            owned_acked += chunk_size;
        } else {
            // The zero-copy path reports acked bytes so the producer can
            // advance its own bookkeeping (e.g. HTTP/3 flow control).
            borrowed_acked += chunk_size;
        }
    }
    if (owned_acked) {
        release_send_bytes(owned_acked);
    }
    if (borrowed_acked && _ack_notifier) {
        _ack_notifier(borrowed_acked);
    }
}

void quic_stream_impl::on_extend_max_stream_data() noexcept {
    set_blocked(false);
}

void quic_stream_impl::on_closed(std::optional<uint64_t> app_error_code) noexcept {
    _closed = true;
    if (app_error_code && !_recv_reset_error && !_recv_fin) {
        _recv_reset_error = app_error_code;
    }
    if (_fin_flushed) {
        if (_fin_submitted) {
            _fin_flushed->set_value();
        } else {
            _fin_flushed->set_exception(std::make_exception_ptr(
                std::system_error(make_error_code(errc::stream_state), "stream closed before FIN was sent")));
        }
        _fin_flushed.reset();
    }
    _recv_cv.broadcast();
}

void quic_stream_impl::on_connection_error(std::exception_ptr ep) noexcept {
    _conn_error = ep;
    _closed = true;
    _send_budget.broken(ep);
    if (_fin_flushed) {
        _fin_flushed->set_exception(ep);
        _fin_flushed.reset();
    }
    _recv_cv.broadcast();
}

// Connection-facing send-queue access.

bool quic_stream_impl::wants_send() const noexcept {
    if (_write_side_reset || _closed) {
        return false;
    }
    return _send_q_bytes > _send_q_submitted || fin_pending();
}

size_t quic_stream_impl::pending_vectors(std::span<ngtcp2_vec> out) const noexcept {
    size_t skip = _send_q_submitted;
    size_t count = 0;
    for (const auto& chunk : _send_q) {
        if (count == out.size()) {
            break;
        }
        if (skip >= chunk.size()) {
            skip -= chunk.size();
            continue;
        }
        out[count].base = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(chunk.view.data())) + skip;
        out[count].len = chunk.size() - skip;
        skip = 0;
        ++count;
    }
    return count;
}

void quic_stream_impl::advance_submitted(size_t n, bool fin_included) noexcept {
    _send_q_submitted += n;
    if (fin_included && !_fin_submitted) {
        _fin_submitted = true;
        if (_fin_flushed) {
            _fin_flushed->set_value();
            _fin_flushed.reset();
        }
    }
}

void quic_stream_impl::release_send_bytes(size_t n) noexcept {
    _send_budget.signal(n);
}

// Iostream plumbing.

namespace {

class quic_source_impl final : public data_source_impl {
    lw_shared_ptr<quic_stream_impl> _stream;
public:
    explicit quic_source_impl(lw_shared_ptr<quic_stream_impl> stream)
        : _stream(std::move(stream)) {
    }
    future<temporary_buffer<char>> get() override {
        return _stream->read_some();
    }
};

class quic_sink_impl final : public data_sink_impl {
    lw_shared_ptr<quic_stream_impl> _stream;
public:
    explicit quic_sink_impl(lw_shared_ptr<quic_stream_impl> stream)
        : _stream(std::move(stream)) {
    }
    future<> put(std::span<temporary_buffer<char>> data) override {
        for (auto& buf : data) {
            co_await _stream->write(std::move(buf));
        }
    }
    future<> close() override {
        return _stream->close_write();
    }
    size_t buffer_size() const noexcept override {
        return 8192;
    }
    bool can_batch_flushes() const noexcept override {
        return false;
    }
};

} // anonymous namespace

data_source quic_stream_impl::make_source() {
    if (_input_taken) {
        throw std::system_error(make_error_code(errc::invalid_state), "stream input already taken");
    }
    if (kind() == stream_kind::unidirectional && is_locally_initiated()) {
        throw std::system_error(make_error_code(errc::invalid_state),
                                "cannot read from a locally-initiated unidirectional stream");
    }
    _input_taken = true;
    return data_source(std::make_unique<quic_source_impl>(shared_from_this()));
}

data_sink quic_stream_impl::make_sink() {
    if (_output_taken) {
        throw std::system_error(make_error_code(errc::invalid_state), "stream output already taken");
    }
    if (kind() == stream_kind::unidirectional && !is_locally_initiated()) {
        throw std::system_error(make_error_code(errc::invalid_state),
                                "cannot write to a peer-initiated unidirectional stream");
    }
    _output_taken = true;
    return data_sink(std::make_unique<quic_sink_impl>(shared_from_this()));
}

input_stream<char> quic_stream_impl::input() {
    return input_stream<char>(make_source());
}

output_stream<char> quic_stream_impl::output() {
    return output_stream<char>(make_sink(), 8192);
}

future<> quic_stream_impl::wait_input_shutdown() {
    co_await _recv_cv.when([this] {
        return _recv_fin || _recv_reset_error.has_value() || _closed || _conn_error;
    });
}

socket_address quic_stream_impl::local_address() const noexcept {
    return _conn->local_address();
}

socket_address quic_stream_impl::remote_address() const noexcept {
    return _conn->remote_address();
}

namespace {

// A QUIC stream presented as a connected_socket: an ordered, reliable,
// flow-controlled bidirectional byte channel, so connected_socket-based
// protocol code runs unchanged over QUIC. TCP-specific socket options do
// not apply per-stream and are inert (see each member).
class quic_connected_socket_impl final : public net::connected_socket_impl {
    lw_shared_ptr<quic_stream_impl> _stream;
public:
    explicit quic_connected_socket_impl(lw_shared_ptr<quic_stream_impl> stream)
        : _stream(std::move(stream)) {
    }
    data_source source() override {
        return _stream->make_source();
    }
    data_sink sink() override {
        return _stream->make_sink();
    }
    void shutdown_input() override {
        // Ask the peer to stop sending; pending reads fail (see
        // quic_stream_impl::on_reset, triggered by the peer's RESET).
        (void)_stream->stop_sending(application_error_code{});
    }
    void shutdown_output() override {
        // Abrupt, per the connected_socket contract ("writes that have not
        // been flushed will immediately fail"): RESET_STREAM. A graceful
        // FIN is sent by closing the socket's output stream instead.
        (void)_stream->reset_write(application_error_code{});
    }
    void set_nodelay(bool) override {
        // QUIC never delays stream data waiting for more (no Nagle).
    }
    bool get_nodelay() const override {
        return true;
    }
    void set_keepalive(bool) override {
        // Keep-alive is a connection-level property in QUIC
        // (connection_config::keep_alive_interval), not per-stream.
    }
    bool get_keepalive() const override {
        return false;
    }
    void set_keepalive_parameters(const net::keepalive_params&) override {
    }
    net::keepalive_params get_keepalive_parameters() const override {
        return net::tcp_keepalive_params{};
    }
    void set_sockopt(int, int, const void*, size_t) override {
        // The UDP socket is shared by every connection on the shard; a
        // per-stream socket option has no meaningful scope.
        throw std::system_error(ENOPROTOOPT, std::system_category(),
                                "socket options not supported on a QUIC stream");
    }
    int get_sockopt(int, int, void*, size_t) const override {
        throw std::system_error(ENOPROTOOPT, std::system_category(),
                                "socket options not supported on a QUIC stream");
    }
    socket_address local_address() const noexcept override {
        return _stream->local_address();
    }
    socket_address remote_address() const noexcept override {
        return _stream->remote_address();
    }
    future<> wait_input_shutdown() override {
        return _stream->wait_input_shutdown();
    }
};

} // anonymous namespace

std::unique_ptr<net::connected_socket_impl> make_connected_socket_impl(lw_shared_ptr<quic_stream_impl> stream) {
    return std::make_unique<quic_connected_socket_impl>(std::move(stream));
}

} // namespace seastar::net::quic::internal
