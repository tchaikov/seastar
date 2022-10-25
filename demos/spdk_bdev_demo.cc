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

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <vector>

#include <seastar/core/app-template.hh>
#include <seastar/core/file.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/spdk_app.hh>
#include <seastar/core/spdk_bdev.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/util/log.hh>

using namespace seastar;
namespace bpo = boost::program_options;

seastar::logger spdk_logger("spdk_demo");

static constexpr int MEMSET_PATTERN = 0x42;

int main(int ac, char** av) {
    seastar::app_template seastar_app;
    spdk::app spdk_app;
    seastar_app.add_options()
        ( "bdev", bpo::value<std::string>()->default_value("Malloc0"),
          "bdev name");
    return seastar_app.run(ac, av, [&] {
        spdk_logger.error("demo running");
        if (!seastar_app.options().smp_opts.use_spdk) {
            std::cerr << "SPDK pmd backend is required to run this application. "
                      << "Please pass '--use-spdk' in the command line arguments.\n";
                return make_ready_future<int>(1);
        }
        auto bdev_name = seastar_app.configuration()["bdev"].as<std::string>();
        return spdk_app.run(seastar_app.options().spdk_opts, [bdev_name] {
            spdk::list_devices all_devices{false};
            if (std::none_of(all_devices.begin(), all_devices.end(),
                             [&] (const std::string& name){
                                 return bdev_name == name;
                             })) {
                std::cerr << "Device " << std::quoted(bdev_name) << " not found. "
                    << "Please specify one of the following device(s): ";
                std::copy(all_devices.begin(),
                          all_devices.end(),
                          std::ostream_iterator<std::string>(std::cerr, ", "));
                std::cerr << std::endl;
                throw std::invalid_argument("unknown device");
            }
            spdk_logger.info("bdev.open");
            std::unique_ptr<spdk::block_device> bdev = spdk::block_device::open(bdev_name);
            uint32_t block_size = bdev->block_size();
            temporary_buffer<char> buf;
            if (size_t alignment = bdev->memory_dma_alignment() == 1) {
                buf = temporary_buffer<char>{block_size};
            } else {
                buf = temporary_buffer<char>::aligned(alignment, block_size);
            }
            return do_with(std::move(bdev),
                           std::move(buf),
                           [] (std::unique_ptr<spdk::block_device>& bdev,
                               temporary_buffer<char>& buf) {
                spdk_logger.info("bdev.write");
                memset(buf.get_write(), MEMSET_PATTERN, buf.size());
                return bdev->write(0, buf.get(), buf.size()).then([&] {
                    memset(buf.get_write(), 0xff, buf.size());
                    spdk_logger.info("bdev.read");
                    return bdev->read(0, buf.get_write(), buf.size());
                }).then([&buf] {
                    temporary_buffer<char> good{buf.size()};
                    memset(good.get_write(), MEMSET_PATTERN, good.size());
                    if (int where = memcmp(good.get(), buf.get(), buf.size());
                        where != 0) {
                        spdk_logger.error("buf mismatches at {}!", where);
                    } else {
                        spdk_logger.info("buf matches!");
                    }
                });
            });
        }).then_wrapped([] (auto f) {
            try {
                f.get();
                return 0;
            } catch (std::exception& e) {
                std::cerr << e.what() << std::endl;
                return 1;
            } catch (...) {
                std::cout << "unknown exception" << std::endl;
                return 1;
            }
        });
    });
}
