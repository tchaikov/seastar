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
 *
 * A minimal HTTP/3 server demo. Serves a "hello" route over HTTP/3.
 * Requires a TLS certificate and key:
 *
 *   ./http3_server_demo --cert server.crt --key server.key --port 4433
 */

#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http3/server.hh>

#include <fmt/printf.h>

using namespace seastar;
namespace bpo = boost::program_options;

int main(int ac, char** av) {
    app_template app;
    app.add_options()
        ("cert", bpo::value<std::string>(), "server certificate (PEM)")
        ("key", bpo::value<std::string>(), "server private key (PEM)")
        ("port", bpo::value<uint16_t>()->default_value(4433), "UDP port");

    return app.run(ac, av, [&app] () -> future<int> {
        auto& cfg = app.configuration();
        if (!cfg.count("cert") || !cfg.count("key")) {
            fmt::print(stderr, "--cert and --key are required\n");
            co_return 1;
        }
        // The credentials builder is copyable across shards; each shard
        // builds its own credentials.
        tls::credentials_builder creds;
        co_await creds.set_x509_key_file(cfg["cert"].as<std::string>(),
                                         cfg["key"].as<std::string>(), tls::x509_crt_format::PEM);

        // One server instance per shard, all bound to the same address:
        // the kernel spreads connections across shards (SO_REUSEPORT) and
        // stray datagrams are forwarded to the owning shard.
        auto server = std::make_unique<experimental::http3::http3_server_control>();
        co_await server->start();
        co_await server->set_routes([] (httpd::routes& r) {
            r.put(httpd::GET, "/hello",
                new httpd::function_handler([] (httpd::const_req) {
                    return sstring("Hello from Seastar HTTP/3!\n");
                }, "txt"));
        });
        co_await server->listen(socket_address(ipv4_addr{"0.0.0.0", cfg["port"].as<uint16_t>()}), creds);
        fmt::print("HTTP/3 server listening on :{} on {} shards (try GET /hello)\n",
                   cfg["port"].as<uint16_t>(), smp::count);

        co_await engine().wait_for_stop(std::chrono::hours(24 * 365));
        co_await server->stop();
        co_return 0;
    });
}
