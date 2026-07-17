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

#include "net/quic/connection.hh"
#include "net/quic/stream.hh"

#include <seastar/core/condition-variable.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/request.hh>
#include <seastar/http3/error.hh>
#include <seastar/net/quic/quic.hh>

#include <nghttp3/nghttp3.h>

#include <deque>
#include <memory>
#include <unordered_map>

namespace seastar::experimental::http3::internal {

using namespace seastar::net;

/// Per-request/response state tracked on an HTTP/3 stream.
struct h3_stream {
    int64_t id;
    lw_shared_ptr<quic::internal::quic_stream_impl> qstream;

    // Header assembly (server: request; client: response).
    std::unique_ptr<http::request> request;
    std::unique_ptr<http::reply> reply;

    // Received body chunks (server: request body; client: response body),
    // delivered to a consumer via an input_stream.
    std::deque<temporary_buffer<char>> recv_body;
    condition_variable recv_cv;
    bool recv_eof = false;
    bool aborted = false;

    // Outgoing body: chunks queued by the producer, consumed by the
    // nghttp3 data reader (pull model). Buffers are retained until acked,
    // then popped. send_queued is the unacknowledged byte count, used to
    // watermark the producer so a large body streams in bounded memory.
    std::deque<temporary_buffer<char>> send_body;
    size_t send_acked = 0;    // bytes at the front chunk already acknowledged
    size_t send_read_off = 0; // bytes from the front already handed to nghttp3
    size_t send_queued = 0;   // unacknowledged body bytes buffered
    bool send_eof = false;    // producer finished
    condition_variable send_cv;

    // Set once headers are complete and the request/response is ready to
    // hand to the consumer.
    bool headers_done = false;
    condition_variable headers_cv;

    explicit h3_stream(int64_t id, lw_shared_ptr<quic::internal::quic_stream_impl> s)
        : id(id), qstream(std::move(s)) {}
};

/// Drives a single nghttp3 connection over a QUIC connection: binds the
/// HTTP/3 control and QPACK streams, pumps received stream bytes into
/// nghttp3, and runs the send fiber that turns nghttp3 output into QUIC
/// stream writes. Subclassed by the server and client connections, which
/// provide the header/data callbacks.
class h3_connection : public enable_shared_from_this<h3_connection> {
public:
    explicit h3_connection(quic::connection conn, bool server);
    virtual ~h3_connection();

    /// Binds control/QPACK streams and starts the driver fibers.
    future<> start();

    /// Closes the HTTP/3 connection (sends GOAWAY, then closes QUIC) and
    /// waits for teardown to finish. Safe to call from a fiber that does
    /// NOT hold this connection's gate (e.g. an external caller); a fiber
    /// that does hold the gate (rx_pump/send_fiber) must use begin_close()
    /// instead, since awaiting the gate's own closure from inside a fiber
    /// that is itself one of the gate's holders would deadlock.
    future<> close(h3_error_code code = h3_error_code::no_error);

    future<> closed() {
        return _closed.get_shared_future();
    }

public:
    h3_stream* find_stream(int64_t id);

    // Forgets a stream once its consumer is done with it, granting QUIC
    // flow-control credit for any body bytes that were received but never
    // consumed (otherwise the connection-level receive window leaks).
    void release_stream(int64_t id);

    // data_source over a received body (server: request body; client:
    // response body). Consuming a chunk grants its bytes back to QUIC flow
    // control, so a slow consumer backpressures the peer instead of letting
    // recv_body grow without bound.
    class body_source final : public data_source_impl {
        shared_ptr<h3_connection> _conn;
        int64_t _id;
    public:
        body_source(shared_ptr<h3_connection> conn, int64_t id)
            : _conn(std::move(conn)), _id(id) {}
        future<temporary_buffer<char>> get() override {
            auto* s = _conn->find_stream(_id);
            while (s && s->recv_body.empty() && !s->recv_eof && !s->aborted) {
                co_await s->recv_cv.when();
                s = _conn->find_stream(_id);
            }
            if (s && s->recv_body.empty() && s->aborted) {
                // Cancelled (local abort or peer reset): fail the read
                // rather than pass off a truncated body as complete.
                throw http3_exception(h3_error_code::request_cancelled, "request aborted");
            }
            if (!s || s->recv_body.empty()) {
                co_return temporary_buffer<char>();
            }
            auto buf = std::move(s->recv_body.front());
            s->recv_body.pop_front();
            s->qstream->consume(buf.size());
            co_return buf;
        }
    };

    // data_sink feeding an outgoing body (server: response; client:
    // request) into the stream's send queue, bounded by a watermark of
    // unacknowledged bytes so a large body streams in bounded memory.
    class body_sink final : public data_sink_impl {
        static constexpr size_t watermark = 256 * 1024;
        shared_ptr<h3_connection> _conn;
        int64_t _id;
    public:
        body_sink(shared_ptr<h3_connection> conn, int64_t id)
            : _conn(std::move(conn)), _id(id) {}
        future<> put(std::span<temporary_buffer<char>> data) override {
            auto* s = _conn->find_stream(_id);
            if (!s) {
                co_return;
            }
            for (auto& b : data) {
                if (b.empty()) {
                    continue;
                }
                s->send_queued += b.size();
                s->send_body.push_back(std::move(b));
            }
            _conn->resume_stream(_id);
            while ((s = _conn->find_stream(_id)) && s->send_queued > watermark && !s->aborted) {
                co_await s->send_cv.when();
            }
        }
        future<> close() override {
            if (auto* s = _conn->find_stream(_id)) {
                s->send_eof = true;
                _conn->resume_stream(_id);
            }
            return make_ready_future<>();
        }
        size_t buffer_size() const noexcept override {
            return 64 * 1024;
        }
    };

    // Resumes a paused stream in nghttp3 (its data reader had returned
    // WOULDBLOCK) and wakes the send fiber. Called when new response-body
    // data becomes available.
    void resume_stream(int64_t id) {
        if (_h3) {
            nghttp3_conn_resume_stream(_h3, id);
        }
        _send_cv.signal();
    }

protected:
    // Wakes the send fiber (new output available).
    void kick_send() {
        _send_cv.signal();
    }
    // Idempotent, non-suspending start of teardown: marks the connection
    // closing, shuts nghttp3 down, and (on the first call) kicks off the
    // actual gate-draining teardown as a detached fiber. Use this instead
    // of close() from rx_pump/send_fiber's own error paths, since those
    // fibers hold _gate themselves — awaiting close()'s _gate.close()
    // from inside one of the gate's own holders can never complete.
    void begin_close(h3_error_code code) noexcept;
    // Registers a stream and starts pumping its received bytes.
    h3_stream& track_stream(int64_t id, lw_shared_ptr<quic::internal::quic_stream_impl> s);
    void start_rx_pump(h3_stream& s);

    // Called for each peer-initiated bidirectional stream (server only).
    virtual void on_new_request_stream(h3_stream& s) {}

    // nghttp3 callbacks common to the server and client connections (the
    // user pointer is the h3_connection-derived object in both).
    static h3_connection& from(void* p) {
        return *static_cast<h3_connection*>(p);
    }
    static int on_recv_data(nghttp3_conn*, int64_t stream_id, const uint8_t* data, size_t datalen,
                            void* user, void*) {
        auto& self = from(user);
        if (auto* s = self.find_stream(stream_id)) {
            s->recv_body.emplace_back(reinterpret_cast<const char*>(data), datalen);
            s->recv_cv.broadcast();
        }
        return 0;
    }
    static int on_end_stream(nghttp3_conn*, int64_t stream_id, void* user, void*) {
        auto& self = from(user);
        if (auto* s = self.find_stream(stream_id)) {
            s->recv_eof = true;
            s->recv_cv.broadcast();
        }
        return 0;
    }
    static int on_acked_stream_data(nghttp3_conn*, int64_t stream_id, uint64_t datalen, void* user, void*) {
        auto& self = from(user);
        if (auto* s = self.find_stream(stream_id)) {
            // Release acknowledged body chunks and wake the producer if the
            // buffered body has dropped below its watermark.
            uint64_t remaining = datalen;
            while (remaining > 0 && !s->send_body.empty()) {
                auto& front = s->send_body.front();
                auto front_size = front.size();
                auto avail = front_size - s->send_acked;
                if (remaining >= avail) {
                    remaining -= avail;
                    s->send_acked = 0;
                    s->send_body.pop_front();
                    // The whole front chunk left; shift the read offset,
                    // which is measured from the front of send_body.
                    s->send_read_off -= std::min(s->send_read_off, front_size);
                } else {
                    s->send_acked += remaining;
                    remaining = 0;
                }
            }
            s->send_queued -= std::min<size_t>(s->send_queued, datalen);
            s->send_cv.broadcast();
        }
        return 0;
    }
    // Pull-model nghttp3 data reader over a stream's send_body queue.
    static nghttp3_ssize read_body_data(nghttp3_conn*, int64_t stream_id, nghttp3_vec* vec, size_t veccnt,
                                        uint32_t* pflags, void* user, void*);

    nghttp3_conn* _h3 = nullptr;
    quic::connection _conn;
    bool _server;
    std::unordered_map<int64_t, std::unique_ptr<h3_stream>> _streams;
    condition_variable _send_cv;
    gate _gate;
    shared_promise<> _closed;
    bool _closing = false;

private:
    future<> setup_uni_streams();
    future<> accept_loop();       // server: accept request streams
    future<> send_fiber();
    future<> rx_pump(h3_stream& s);
    // The gate-draining tail of teardown, run detached by begin_close();
    // keeps `this` alive via shared_from_this() across its awaits.
    future<> finish_close(h3_error_code code);

    // The transport's shared timestamp source.
    static nghttp3_tstamp now() noexcept {
        return quic::internal::quic_now();
    }
};

} // namespace seastar::experimental::http3::internal
