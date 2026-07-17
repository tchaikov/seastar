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

#include "h3_connection.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/util/log.hh>

#include <utility>

namespace seastar::experimental::http3 {

logger h3_log("seastar-http3");

namespace internal {

namespace {

// If [base, base+len) lies within one of the stream's response-body chunks,
// return a share of that chunk (same memory, shared ownership) so it can be
// sent by reference and kept alive independently of send_body. Otherwise
// return an empty buffer, signalling nghttp3-internal framing to be copied.
temporary_buffer<char> borrow_body_chunk(h3_stream& s, const char* base, size_t len) {
    for (auto& chunk : s.send_body) {
        auto* begin = chunk.get();
        if (base >= begin && base + len <= begin + chunk.size()) {
            return chunk.share();
        }
    }
    return {};
}

} // namespace

h3_connection::h3_connection(quic::connection conn, bool server)
    : _conn(std::move(conn))
    , _server(server) {
}

h3_connection::~h3_connection() {
    if (_h3) {
        nghttp3_conn_del(_h3);
        _h3 = nullptr;
    }
}

h3_stream* h3_connection::find_stream(int64_t id) {
    auto it = _streams.find(id);
    return it == _streams.end() ? nullptr : it->second.get();
}

void h3_connection::release_stream(int64_t id) {
    auto it = _streams.find(id);
    if (it == _streams.end()) {
        return;
    }
    auto& st = *it->second;
    // Body bytes are credited on consumption; return whatever the consumer
    // never popped, or the receive window would leak.
    size_t unconsumed = 0;
    for (const auto& chunk : st.recv_body) {
        unconsumed += chunk.size();
    }
    if (unconsumed > 0) {
        st.qstream->consume(unconsumed);
    }
    _streams.erase(it);
}

nghttp3_ssize h3_connection::read_body_data(nghttp3_conn*, int64_t stream_id, nghttp3_vec* vec, size_t veccnt,
                                        uint32_t* pflags, void* user, void*) {
    auto& self = from(user);
    auto* s = self.find_stream(stream_id);
    if (!s) {
        *pflags = NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }
    // Present body bytes not yet handed to nghttp3 (from send_read_off),
    // as pointers into send_body (no copy). nghttp3 references them
    // until they are acknowledged, at which point acked_stream_data
    // drains them.
    size_t skip = s->send_read_off;
    size_t n = 0;
    size_t returned = 0;
    bool served_all = true;
    for (auto& chunk : s->send_body) {
        if (n == veccnt) {
            served_all = false; // more chunks remain than we can return now
            break;
        }
        if (skip >= chunk.size()) {
            skip -= chunk.size();
            continue;
        }
        vec[n].base = reinterpret_cast<uint8_t*>(chunk.get_write()) + skip;
        vec[n].len = chunk.size() - skip;
        returned += chunk.size() - skip;
        skip = 0;
        ++n;
    }
    if (n == 0) {
        if (s->send_eof) {
            *pflags = NGHTTP3_DATA_FLAG_EOF;
            return 0;
        }
        return NGHTTP3_ERR_WOULDBLOCK;
    }
    s->send_read_off += returned;
    // Only end the stream once the whole body has been handed over.
    if (s->send_eof && served_all) {
        *pflags = NGHTTP3_DATA_FLAG_EOF;
    }
    return static_cast<nghttp3_ssize>(n);
}

h3_stream& h3_connection::track_stream(int64_t id, lw_shared_ptr<quic::internal::quic_stream_impl> s) {
    auto [it, _] = _streams.emplace(id, std::make_unique<h3_stream>(id, std::move(s)));
    auto& hs = *it->second;
    // When the transport acknowledges zero-copy-sent bytes, inform nghttp3
    // so it releases its framing buffers and fires acked_stream_data
    // (which drains our response-body queue). The notifier is owned by the
    // stream, so it never outlives this connection.
    hs.qstream->set_ack_notifier([this, id] (size_t delta) {
        if (_h3) {
            nghttp3_conn_add_ack_offset(_h3, id, delta);
            kick_send();
        }
    });
    return hs;
}

future<> h3_connection::start() {
    co_await setup_uni_streams();
    (void)accept_loop().handle_exception([] (std::exception_ptr ep) {
        h3_log.debug("h3 accept loop failed: {}", ep);
    }).finally([self = shared_from_this()] {});
    (void)send_fiber().handle_exception([] (std::exception_ptr ep) {
        h3_log.debug("h3 send fiber failed: {}", ep);
    }).finally([self = shared_from_this()] {});
}

future<> h3_connection::setup_uni_streams() {
    // Open the local control and QPACK encoder/decoder unidirectional
    // streams and bind them to nghttp3.
    auto ctrl = co_await _conn.open_stream(quic::stream_kind::unidirectional);
    auto qenc = co_await _conn.open_stream(quic::stream_kind::unidirectional);
    auto qdec = co_await _conn.open_stream(quic::stream_kind::unidirectional);

    auto ctrl_id = ctrl.id();
    auto qenc_id = qenc.id();
    auto qdec_id = qdec.id();

    track_stream(ctrl_id, quic::internal::api_access::impl(ctrl));
    track_stream(qenc_id, quic::internal::api_access::impl(qenc));
    track_stream(qdec_id, quic::internal::api_access::impl(qdec));

    if (nghttp3_conn_bind_control_stream(_h3, ctrl_id) != 0) {
        throw http3_exception(h3_error_code::internal_error, "bind_control_stream");
    }
    if (nghttp3_conn_bind_qpack_streams(_h3, qenc_id, qdec_id) != 0) {
        throw http3_exception(h3_error_code::internal_error, "bind_qpack_streams");
    }
    kick_send();
}

future<> h3_connection::accept_loop() {
    auto hold = _gate.hold();
    while (!_closing) {
        quic::stream qs;
        try {
            qs = co_await _conn.accept_stream();
        } catch (...) {
            break;
        }
        auto id = qs.id();
        auto& s = track_stream(id, quic::internal::api_access::impl(qs));
        if (qs.kind() == quic::stream_kind::bidirectional && _server) {
            on_new_request_stream(s);
        }
        start_rx_pump(s);
    }
}

void h3_connection::start_rx_pump(h3_stream& s) {
    (void)rx_pump(s).handle_exception([] (std::exception_ptr ep) {
        h3_log.trace("h3 rx pump failed: {}", ep);
    }).finally([self = shared_from_this()] {});
}

future<> h3_connection::rx_pump(h3_stream& s) {
    auto hold = _gate.hold();
    // Hold a reference of our own: release_stream() may drop the h3_stream
    // (and its qstream reference) while this fiber is suspended in
    // receive().
    auto qstream = s.qstream;
    auto id = s.id;
    while (!_closing) {
        quic::internal::quic_stream_impl::recv_chunk chunk;
        try {
            chunk = co_await qstream->receive();
        } catch (...) {
            // Peer reset the stream, or the connection ended.
            if (auto* st = find_stream(id)) {
                st->aborted = true;
                st->recv_eof = true;
                st->recv_cv.broadcast();
                st->headers_done = true;
                st->headers_cv.broadcast();
            }
            break;
        }
        auto consumed = nghttp3_conn_read_stream2(_h3, id,
            reinterpret_cast<const uint8_t*>(chunk.data.get()), chunk.data.size(), chunk.fin, now());
        if (consumed < 0) {
            h3_log.debug("nghttp3_conn_read_stream2 failed on stream {}: {}", id,
                         nghttp3_strerror(static_cast<int>(consumed)));
            // begin_close(), not close(): this fiber holds _gate itself, and
            // close() awaits _gate.close(), which cannot complete until this
            // fiber returns.
            begin_close(h3_error_code::general_protocol_error);
            break;
        }
        // Credit only the framing bytes nghttp3 consumed (its return value
        // excludes DATA payload). Body bytes are credited when the consumer
        // pops them (body_source) or when the stream is released, so a slow
        // consumer backpressures the peer via the QUIC receive window.
        qstream->consume(static_cast<size_t>(consumed));
        kick_send();
        if (chunk.fin) {
            break;
        }
    }
}

future<> h3_connection::send_fiber() {
    auto hold = _gate.hold();
    std::array<nghttp3_vec, 16> vec;
    while (!_closing) {
        int64_t stream_id = -1;
        int fin = 0;
        auto sveccnt = nghttp3_conn_writev_stream(_h3, &stream_id, &fin, vec.data(), vec.size());
        if (sveccnt < 0) {
            h3_log.debug("nghttp3_conn_writev_stream failed: {}", nghttp3_strerror(static_cast<int>(sveccnt)));
            // See the matching comment in rx_pump(): this fiber holds
            // _gate itself, so it must not await close()'s _gate.close().
            begin_close(h3_error_code::internal_error);
            break;
        }
        if (sveccnt == 0 && stream_id < 0) {
            // Nothing to send right now; wait for more output.
            co_await _send_cv.when();
            continue;
        }

        auto* s = find_stream(stream_id);
        if (!s) {
            // Stream vanished; tell nghttp3 we consumed nothing further.
            nghttp3_conn_add_write_offset(_h3, stream_id, 0);
            continue;
        }

        // Hand nghttp3's output to the transport for a zero-copy send whose
        // referenced memory is owned by a single keepalive (mirroring the
        // posix TCP data_sink's deleter), so it needs no external lifetime
        // guarantee. nghttp3's writev output interleaves two kinds of bytes:
        //   * DATA-frame payload, which points into our response-body chunks
        //     (send_body) — sent by reference, with a share of the chunk
        //     added to the keepalive;
        //   * framing (HEADERS/DATA frame headers, control-stream bytes),
        //     which nghttp3 owns internally — copied into the keepalive (it
        //     is small), so the transport never references nghttp3 memory.
        // The keepalive is released only once every byte of this batch has
        // been acknowledged. add_ack_offset stays deferred to the QUIC ack
        // (see the notifier in track_stream) so the body producer's
        // watermark tracks real acknowledgement.
        size_t total = 0;
        std::array<std::span<const char>, 16> spans;
        std::vector<temporary_buffer<char>> keep;
        keep.reserve(static_cast<size_t>(sveccnt));
        for (nghttp3_ssize i = 0; i < sveccnt; ++i) {
            auto* base = reinterpret_cast<const char*>(vec[i].base);
            size_t len = vec[i].len;
            total += len;
            if (auto ref = borrow_body_chunk(*s, base, len); ref.size()) {
                spans[i] = std::span<const char>(base, len);
                keep.push_back(std::move(ref));
            } else {
                temporary_buffer<char> copy(base, len);
                spans[i] = std::span<const char>(copy.get(), copy.size());
                keep.push_back(std::move(copy));
            }
        }
        auto keepalive = make_deleter([keep = std::move(keep)] () mutable { keep.clear(); });
        s->qstream->send_borrowed({spans.data(), static_cast<size_t>(sveccnt)}, fin != 0, std::move(keepalive));
        nghttp3_conn_add_write_offset(_h3, stream_id, total);
    }
}

void h3_connection::begin_close(h3_error_code code) noexcept {
    if (_closing) {
        return;
    }
    _closing = true;
    if (_h3) {
        nghttp3_conn_shutdown(_h3);
    }
    _send_cv.broadcast();
    // Run the gate-draining teardown detached: rx_pump/send_fiber call
    // begin_close() from within their own gate hold, and awaiting
    // _gate.close() synchronously here would deadlock against that hold
    // (it cannot reach zero until the calling fiber itself returns).
    // Detaching lets this fiber's caller return and release its hold
    // first; finish_close() then waits for every remaining holder.
    (void)finish_close(code).handle_exception([] (std::exception_ptr ep) {
        h3_log.debug("h3 connection close cleanup failed: {}", ep);
    });
}

future<> h3_connection::finish_close(h3_error_code code) {
    auto self = shared_from_this();
    // Give the send fiber a chance to flush GOAWAY, then close the QUIC
    // connection.
    co_await _conn.close(quic::application_error_code{std::to_underlying(code)}, {});
    co_await _gate.close();
    _closed.set_value();
}

future<> h3_connection::close(h3_error_code code) {
    begin_close(code);
    co_await _closed.get_shared_future();
}

} // namespace internal
} // namespace seastar::experimental::http3
