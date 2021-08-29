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

#include <cstring>
#include <limits>
#include <random>

#include <seastar/core/app-template.hh>

#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/file.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/spdk_app.hh>
#include <seastar/core/spdk_bdev.hh>
#include <seastar/core/spdk_lib.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/util/log.hh>
#include <seastar/util/tmp_file.hh>

using namespace seastar;
namespace bpo = boost::program_options;

seastar::logger spdk_logger("spdk_demo");

int main(int ac, char** av) {
    seastar::app_template seastar_app;
    seastar_app.add_positional_options({
        { "bdev", bpo::value<std::string>()->default_value("Malloc0"),
          "bdev", 1 },
    });
    spdk::app spdk_app;
    spdk_logger.info("app run");
    return seastar_app.run(ac, av, [&] {
        spdk_logger.info("demo running");
        auto bdev_name = seastar_app.configuration()["bdev"].as<std::string>();
        return spdk_app.run(seastar_app.configuration(), [bdev_name] {
            spdk_logger.info("bdev.open");
            auto bdev = spdk::block_device::open(bdev_name);
            uint32_t block_size = bdev->block_size();
            size_t buf_align = bdev->memory_dma_alignment();
            auto buf = spdk::dma_zmalloc(block_size, buf_align);
            return do_with(temporary_buffer<char>(std::move(buf)),
                           std::unique_ptr<spdk::block_device>(std::move(bdev)),
                           [] (temporary_buffer<char>& buf,
                               std::unique_ptr<spdk::block_device>& bdev) {
                spdk_logger.info("bdev.write");
                return bdev->write(0, buf.get(), buf.size()).then([&] {
                    spdk_logger.info("bdev.read");
                    memset(buf.get_write(), 0xff, buf.size());
                    return bdev->read(0, buf.get_write(), buf.size());
                }).then([&buf] {
                    spdk_logger.info("bdev.read");
                    temporary_buffer<char> good{buf.size()};
                    memset(good.get_write(), 0, good.size());
                    if (int where = memcmp(good.get(), buf.get(), buf.size());
                        where != 0) {
                        spdk_logger.error("buf mismatches at {}!", where);
                    } else {
                        spdk_logger.info("buf matches!");
                    }
                });
            }).handle_exception_type([&] (std::system_error& e) {
                spdk_logger.error("error while writing/reading {}", e.what());
            });
        });
    });
}
