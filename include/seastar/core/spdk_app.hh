// -*- mode:C++; tab-width:8; c-basic-offset:4; indent-tabs-mode:nil -*-
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
 * Copyright (C) 2021 Kefu Chai <tchaikov@gmail.com>
 */

#pragma once

#ifdef SEASTAR_HAVE_SPDK

#include <boost/program_options.hpp>
#include <seastar/core/future.hh>
#include <seastar/core/resource.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/spdk_thread.hh>

struct spdk_thread;

namespace seastar::spdk {

struct options;

namespace env {
    void start(const std::vector<resource::cpu>& cpuset,
               const options& opts);
    void stop() noexcept;
};

// Helper to setup and tear down the SPDK environment
//
// \c app
// -# accepts a bunch of command line options which mirror the ones
//    recognized by \c spdk_app_parse_args().
// -# initializes the SPDK subsystems specified by the options.
// -# starts an RPC server offering JSON-RPC remote access.
class app {
public:
    future<int> run(const options& opts,
                    std::function<future<> ()>&& func) noexcept;
private:
    future<> start(const options& opts);
    future<> stop();
private:
    sharded<executor> sharded_executor;
    spdk_thread* app_thread = nullptr;
};

}

#endif // SEASTAR_HAVE_SPDK
