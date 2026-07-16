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

#include "connection.hh"
#include "server.hh"
#include "stream.hh"

#include <seastar/net/stack.hh>

#include <seastar/core/coroutine.hh>

namespace seastar::net::quic {

// stream

stream::stream() noexcept = default;
stream::stream(lw_shared_ptr<internal::quic_stream_impl> impl) noexcept : _impl(std::move(impl)) {}
stream::stream(stream&&) noexcept = default;
stream& stream::operator=(stream&&) noexcept = default;
stream::~stream() = default;

stream_id stream::id() const noexcept {
    return _impl->id();
}

stream_kind stream::kind() const noexcept {
    return _impl->kind();
}

input_stream<char> stream::input() {
    return _impl->input();
}

output_stream<char> stream::output() {
    return _impl->output();
}

connected_socket stream::to_connected_socket() && {
    if (!_impl) {
        throw std::system_error(make_error_code(errc::invalid_state), "empty stream");
    }
    if (_impl->kind() != stream_kind::bidirectional) {
        throw std::system_error(make_error_code(errc::invalid_state),
                                "only a bidirectional stream can become a connected_socket");
    }
    return connected_socket(internal::make_connected_socket_impl(std::move(_impl)));
}

std::expected<void, std::error_code> stream::reset_write(application_error_code code) noexcept {
    return _impl->reset_write(code);
}

std::expected<void, std::error_code> stream::stop_sending(application_error_code code) noexcept {
    return _impl->stop_sending(code);
}

// connection

connection::connection() noexcept = default;
connection::connection(lw_shared_ptr<internal::quic_connection_impl> impl) noexcept : _impl(std::move(impl)) {}
connection::connection(connection&&) noexcept = default;
connection& connection::operator=(connection&&) noexcept = default;
connection::~connection() = default;

future<stream> connection::open_stream(stream_kind kind) {
    return _impl->open_stream(kind);
}

future<stream> connection::accept_stream() {
    return _impl->accept_stream();
}

future<> connection::send_datagram(temporary_buffer<char> datagram) {
    return _impl->send_datagram(std::move(datagram));
}

future<temporary_buffer<char>> connection::receive_datagram() {
    return _impl->receive_datagram();
}

future<> connection::close(application_error_code code, sstring reason) noexcept {
    return _impl->close(code, std::move(reason));
}

future<> connection::wait_closed() noexcept {
    return _impl->wait_closed();
}

bool connection::is_closed() const noexcept {
    return _impl->is_closed();
}

std::optional<sstring> connection::alpn() const {
    return _impl->alpn();
}

socket_address connection::local_address() const {
    return _impl->local_address();
}

socket_address connection::remote_address() const {
    return _impl->remote_address();
}

future<> connection::migrate(socket_address new_local, bool immediate) {
    return _impl->migrate(new_local, immediate);
}

future<session_ticket> connection::wait_for_session_ticket() {
    return _impl->wait_for_session_ticket();
}

std::optional<address_token> connection::take_address_token() noexcept {
    return _impl->take_address_token();
}

connection_stats connection::get_stats() const noexcept {
    return _impl->get_stats();
}

// server

server::server() noexcept = default;
server::server(lw_shared_ptr<internal::server_dispatcher> impl) noexcept : _impl(std::move(impl)) {}
server::server(server&&) noexcept = default;
server& server::operator=(server&&) noexcept = default;
server::~server() = default;

future<connection> server::accept() {
    return _impl->accept();
}

void server::abort_accept() {
    _impl->abort_accept();
}

socket_address server::local_address() const {
    return _impl->local_address();
}

future<> server::stop(application_error_code code) noexcept {
    return _impl->stop(code);
}

// free functions

server listen(socket_address addr, shared_ptr<tls::server_credentials> creds, listen_options options) {
    auto dispatcher = make_lw_shared<internal::server_dispatcher>(addr, std::move(creds), std::move(options));
    dispatcher->start();
    return server(std::move(dispatcher));
}

future<connection> connect(socket_address remote, shared_ptr<tls::certificate_credentials> creds,
                           connect_options options) {
    auto impl = co_await internal::quic_connection_impl::make_client(remote, std::move(creds), std::move(options));
    co_await impl->wait_handshake();
    co_return connection(std::move(impl));
}

} // namespace seastar::net::quic
