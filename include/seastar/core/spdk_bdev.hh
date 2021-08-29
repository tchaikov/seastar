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

#include <memory>
#include <seastar/core/future.hh>

struct spdk_bdev;
struct spdk_bdev_desc;
struct spdk_io_channel;

namespace seastar::spdk {

class block_device {
public:
    static std::unique_ptr<block_device> open(const std::string& bdev_name);
    ~block_device();

    future<> write(uint64_t pos, const void* buffer, size_t len);
    future<> read(uint64_t pos, void* buffer, size_t len);

    uint32_t block_size() const;
    size_t memory_dma_alignment() const;

private:
    block_device() = default;
    static void event_cb(int /* spdk_bdev_event_type */ type,
                         struct spdk_bdev* bdev,
                         void* event_ctx);

private:
    spdk_bdev* _bdev = nullptr;
    spdk_bdev_desc* _desc = nullptr;
    spdk_io_channel* _io_channel = nullptr;
};

}
