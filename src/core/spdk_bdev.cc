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
#ifdef SEASTAR_HAVE_SPDK

#include <memory>
#include <numeric>

#include <seastar/core/do_with.hh>
#include <seastar/core/spdk_bdev.hh>
#include <seastar/util/log.hh>
#include <spdk/bdev.h>
#include <spdk/string.h>
#include <spdk/thread.h>

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

class stat_completion_desc {
    seastar::promise<seastar::spdk::block_device::io_stats> _pr;
public:
    void complete_with(spdk_bdev_io_stat* stat, int rc) {
        assert(stat);
        if (rc == 0) {
            seastar::spdk::block_device::io_stats io_stats;
            static_assert(sizeof(io_stats) == sizeof(*stat));
            memcpy(&io_stats, stat, sizeof(io_stats));
            _pr.set_value(std::move(io_stats));
        } else {
            _pr.set_exception(
                std::system_error(-rc,
                                  std::system_category(),
                                  "bdev stat error"));
        }
        delete this;
    }
    seastar::future<seastar::spdk::block_device::io_stats> get_future() {
        return _pr.get_future();
    }
};

}

namespace seastar::spdk {

extern logger logger;

block_device::~block_device()
{
    if (_io_channel) {
        spdk_put_io_channel(_io_channel);
    }
    if (_desc) {
        spdk_bdev_close(_desc);
    }
}

std::unique_ptr<block_device> block_device::open(const std::string& bdev_name)
{
    std::unique_ptr<block_device> bdev{new block_device};
    int rc = spdk_bdev_open_ext(bdev_name.c_str(),
                                true,
                                reinterpret_cast<spdk_bdev_event_cb_t>(event_cb),
                                bdev.get(),
                                &bdev->_desc);
    if (rc) {
        logger.error("unable to open bdev {}: {}",
                     bdev_name, spdk_strerror(-rc));
        throw std::runtime_error(fmt::format("unable to open bdev {}", bdev_name));
    }
    bdev->_bdev = spdk_bdev_desc_get_bdev(bdev->_desc);
    bdev->_io_channel = spdk_bdev_get_io_channel(bdev->_desc);
    if (bdev->_io_channel == nullptr) {
        logger.error("unable to open bdev I/O channel");
        throw std::runtime_error(fmt::format("unable to open io channel"));
    }
    return bdev;
}

void block_device::event_cb(int type, spdk_bdev* bdev, void* event_ctx)
{}

static void spdk_bdev_io_cpl(spdk_bdev_io* bdev_io, bool success, void* arg)
{
    logger.trace("io done");
    auto* desc = static_cast<io_completion_desc*>(arg);
    desc->complete_with(bdev_io, success);
}

future<> block_device::write(uint64_t pos, const void* buffer, size_t len)
{
    assert(_bdev);
    logger.info("write({}, {})", pos, len);
    auto io_desc = std::make_unique<io_completion_desc>();
    auto io_done = io_desc->get_future();
    int rc = spdk_bdev_write(_desc, _io_channel,
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

future<> block_device::writev(uint64_t pos, std::vector<iovec> iov)
{
    auto len = std::accumulate(iov.begin(), iov.end(), size_t(0),
                               [](size_t sum, const iovec& iov) {
                                   return sum + iov.iov_len;
                               });
    auto io_desc = std::make_unique<io_completion_desc>();
    auto io_done = io_desc->get_future();
    int rc = spdk_bdev_writev(_desc, _io_channel,
                              iov.data(), iov.size(), pos, len,
                              spdk_bdev_io_cpl, io_desc.release());
    if (rc == -ENOMEM) {
        io_desc->fail_with(std::bad_alloc());
    } else {
        // -EBADF or -EINVAL
        io_desc->fail_with(std::invalid_argument("out of range"));
    }
    return io_done.finally([iov = std::move(iov)] () {});
}

future<> block_device::read(uint64_t pos, void* buffer, size_t len)
{
    assert(_bdev);
    auto io_desc = std::make_unique<io_completion_desc>();
    auto io_done = io_desc->get_future();
    int rc = spdk_bdev_read(_desc, _io_channel, buffer, pos, len,
                            spdk_bdev_io_cpl, io_desc.release());
    if (rc == 0) {
        return io_done;
    }
    if (rc == -ENOMEM) {
        io_desc->fail_with(std::bad_alloc());
    } else {
        // -EINVAL
        io_desc->fail_with(std::invalid_argument("out of range"));
    }
    return io_done;
}

future<> block_device::readv(uint64_t pos, std::vector<iovec> iov)
{
    auto len = std::accumulate(iov.begin(), iov.end(), size_t(0),
                               [](size_t sum, const iovec& iov) {
                                   return sum + iov.iov_len;
                               });
    auto io_desc = std::make_unique<io_completion_desc>();
    auto io_done = io_desc->get_future();
    int rc = spdk_bdev_readv(_desc, _io_channel,
                             iov.data(), iov.size(), pos, len,
                             spdk_bdev_io_cpl, io_desc.release());
    if (rc == -ENOMEM) {
        io_desc->fail_with(std::bad_alloc());
    } else {
        // -EINVAL
        io_desc->fail_with(std::invalid_argument("out of range"));
    }
    return io_done.finally([iov = std::move(iov)] () {});
}

static void spdk_bdev_get_device_stat_cpl(spdk_bdev* bdev,
                                          spdk_bdev_io_stat* stat,
                                          void* arg, int rc)
{
    logger.trace("stat done");
    auto* desc = static_cast<stat_completion_desc*>(arg);
    desc->complete_with(stat, rc);
}

future<block_device::io_stats> block_device::stat()
{
    return do_with(spdk_bdev_io_stat{}, [this] (spdk_bdev_io_stat& stat) {
        auto stat_desc = std::make_unique<stat_completion_desc>();
        auto stat_done = stat_desc->get_future();
        spdk_bdev_get_device_stat(_bdev, &stat,
                                  spdk_bdev_get_device_stat_cpl,
                                  stat_desc.release());
        return stat_done;
    });
}

future<> block_device::flush(uint64_t pos, size_t len)
{
    assert(_bdev);
    auto io_desc = std::make_unique<io_completion_desc>();
    auto io_done = io_desc->get_future();
    int rc = spdk_bdev_flush(_desc, _io_channel, pos, len,
                             spdk_bdev_io_cpl, io_desc.release());
    if (rc == 0) {
        return io_done;
    }
    if (rc == -ENOMEM) {
        io_desc->fail_with(std::bad_alloc());
    } else {
        // -EINVAL or -EBADF
        io_desc->fail_with(std::invalid_argument(spdk_strerror(-rc)));
    }
    return io_done;
}

uint32_t block_device::block_size() const
{
    assert(_bdev);
    return spdk_bdev_get_block_size(_bdev);
}

size_t block_device::memory_dma_alignment() const
{
    assert(_bdev);
    return spdk_bdev_get_buf_align(_bdev);
}

void dev_iterator::advance() noexcept
{
    assert(_bdev);
    if (_with_vbdev) {
        _bdev = spdk_bdev_next(_bdev);
    } else {
        _bdev = spdk_bdev_next_leaf(_bdev);
    }
}

std::string dev_iterator::operator*() const noexcept
{
    assert(_bdev);
    return spdk_bdev_get_name(_bdev);
}

dev_iterator list_devices::begin() const
{
    spdk_bdev* bdev = nullptr;
    if (_with_vbdev) {
        bdev = spdk_bdev_first();
    } else {
        bdev = spdk_bdev_first_leaf();
    }
    return dev_iterator{bdev, _with_vbdev};
}

dev_iterator list_devices::end() const
{
    return dev_iterator{nullptr, _with_vbdev};
}

}
#endif // SEASTAR_HAVE_SPDK
