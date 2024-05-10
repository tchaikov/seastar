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
 * Copyright (C) 2024 Scylladb, Ltd.
 */

#include <memory>
#include <fmt/core.h>

#include <seastar/http/httpd.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http/file_handler.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/reactor.hh>
#include <seastar/http/api_docs.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/thread.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/util/defer.hh>
#include "../../apps/lib/stop_signal.hh"
#include "api.json.hh"

namespace bpo = boost::program_options;

using namespace seastar;
using namespace httpd;

void set_routes(routes& r) {
    api_json::hello_world.set(r, [] (const_req req) {
        api_json::my_object obj;
        obj.var1 = req.param.at("var1");
        obj.var2 = req.param.at("var2");
        api_json::ns_hello_world::query_enum v = api_json::ns_hello_world::str2query_enum(req.query_parameters.at("query_enum"));
        // This demonstrate enum conversion
        obj.enum_var = v;
        return obj;
    });
}

int main(int ac, char** av) {
    app_template app;

    app.add_options()("port", bpo::value<uint16_t>()->default_value(10000), "HTTP Server port");
    app.add_options()("prometheus_port", bpo::value<uint16_t>()->default_value(9180), "Prometheus port. Set to zero in order to disable.");
    app.add_options()("prometheus_address", bpo::value<sstring>()->default_value("0.0.0.0"), "Prometheus address");
    app.add_options()("prometheus_prefix", bpo::value<sstring>()->default_value("rest_api"), "Prometheus metrics prefix");

    return app.run(ac, av, [&] {
        return seastar::async([&] {
            seastar_apps_lib::stop_signal stop_signal;
            auto&& config = app.configuration();

            httpd::http_server_control prometheus_server;
            bool prometheus_started = false;

            auto stop_prometheus = defer([&] () noexcept {
                if (prometheus_started) {
                    prometheus_server.stop().get();
                }
            });

            uint16_t prometheus_port = config["prometheus_port"].as<uint16_t>();
            if (prometheus_port) {
                prometheus_server.start("prometheus").get();

                prometheus::config prometheus_config;
                prometheus_config.prefix = config["prometheus_prefix"].as<sstring>();
                prometheus::start(prometheus_server, prometheus_config).get();
                prometheus_started = true;

                const net::inet_address prometheus_addr(config["prometheus_address"].as<sstring>());
                const socket_addr addr{prometheus_addr, prometheus_port};
                prometheus_server.listen(addr).handle_exception([addr] (auto ep) {
                    seastar::print(stderr, "Could not start exporter on {}: {}\n", addr, ep);
                    return make_exception_future<>(ep);
                }).get();

            }

            uint16_t port = config["port"].as<uint16_t>();
            auto server = std::make_unique<http_server_control>();
            auto rb = make_shared<api_registry_builder>("apps/httpd/");
            server->start().get();

            auto stop_server = defer([&] () noexcept {
                std::cout << "Stoppping HTTP server" << std::endl; // This can throw, but won't.
                server->stop().get();
            });

            server->set_routes(set_routes).get();
            server->set_routes([rb](routes& r){rb->set_api_doc(r);}).get();
            server->set_routes([rb](routes& r) {rb->register_function(r, "demo", "rest api test");}).get();
            server->listen(port).get();

            fmt::print("{}\n", port);
            fflush(stdout);

            stop_signal.wait().get();
            return 0;
        });
    });
}
