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

#include <seastar/core/spdk_bdev.hh>
#include <seastar/util/log.hh>
#include <spdk/bdev.h>
#include <spdk/string.h>
#include <spdk/thread.h>
#include <memory>

namespace {

class io_completion_desc {
    seastar::promise<> _pr;
public:
    void complete_with(struct spdk_bdev_io* bdev_io, bool success) {
        if (success) {
            _pr.set_value();
        } else {
            _pr.set_exception(
                std::system_error(ECONNABORTED,
                                  std::system_category(),
                                  "bdev IO error"));
        }
        if (bdev_io != nullptr) {
            spdk_bdev_free_io(bdev_io);
        }
        delete this;
    }
    template <class E>
    void fail_with(E&& e) {
        _pr.set_exception(std::make_exception_ptr(std::move(e)));
    }
    seastar::future<> get_future() {
        return _pr.get_future();
    }
};

}

namespace seastar::spdk {

extern logger logger;

block_device::~block_device()
{
    if (io_channel) {
        spdk_put_io_channel(io_channel);
    }
    if (desc) {
        spdk_bdev_close(desc);
    }
}

std::unique_ptr<block_device> block_device::open(const std::string& bdev_name)
{
    std::unique_ptr<block_device> bdev{new block_device};
    int rc = spdk_bdev_open_ext(bdev_name.c_str(),
                                true,
                                reinterpret_cast<spdk_bdev_event_cb_t>(event_cb),
                                bdev.get(),
                                &bdev->desc);
    if (rc) {
        logger.error("unable to open bdev {}: {}",
                     bdev_name, spdk_strerror(-rc));
        throw std::runtime_error(fmt::format("unable to open bdev {}", bdev_name));
    }
    bdev->bdev = spdk_bdev_desc_get_bdev(bdev->desc);
    bdev->io_channel = spdk_bdev_get_io_channel(bdev->desc);
    if (bdev->io_channel == nullptr) {
        logger.error("unable to open bdev I/O channel");
        throw std::runtime_error(fmt::format("unable to open io channel"));
    }
    return bdev;
}

void block_device::event_cb(int type, spdk_bdev* bdev, void* event_ctx)
{}

static void spdk_bdev_io_cpl(spdk_bdev_io* bdev_io, bool success, void* arg)
{
    logger.info("io done");
    auto* desc = static_cast<io_completion_desc*>(arg);
    desc->complete_with(bdev_io, success);
}

future<> block_device::write(uint64_t pos, const void* buffer, size_t len)
{
    assert(bdev);
    logger.info("write({}, {})", pos, len);
    auto io_desc = std::make_unique<io_completion_desc>();
    auto io_done = io_desc->get_future();
    int rc = spdk_bdev_write(desc, io_channel,
                             const_cast<void*>(buffer), pos, len,
                             spdk_bdev_io_cpl, io_desc.release());
    if (rc == 0) {
        return io_done;
    }
    if (rc == -ENOMEM) {
        io_desc->fail_with(std::bad_alloc());
    } else {
        // -EBADF or -EINVAL
        io_desc->fail_with(std::invalid_argument("out of range"));
    }
    return io_done;
}

future<> block_device::read(uint64_t pos, void* buffer, size_t len)
{
    assert(bdev);
    auto io_desc = std::make_unique<io_completion_desc>();
    auto io_done = io_desc->get_future();
    int rc = spdk_bdev_read(desc, io_channel, buffer, pos, len,
                            spdk_bdev_io_cpl, io_desc.release());
    if (rc == 0) {
        return io_done;
    }
    if (rc == -ENOMEM) {
        io_desc->fail_with(std::bad_alloc());
    } else {
        // --EINVAL
        io_desc->fail_with(std::invalid_argument("out of range"));
    }
    return io_done;
}

uint32_t block_device::block_size() const
{
    assert(bdev);
    return spdk_bdev_get_block_size(bdev);
}

size_t block_device::memory_dma_alignment() const
{
    assert(bdev);
    return spdk_bdev_get_buf_align(bdev);
}

}
