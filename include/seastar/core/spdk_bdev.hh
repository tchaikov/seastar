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

#include <seastar/core/future.hh>
#include <iterator>
#include <memory>
#include <sys/uio.h>

struct spdk_bdev;
struct spdk_bdev_desc;
struct spdk_io_channel;

namespace seastar::spdk {

class block_device {
public:
    /// mirrors spdk_bdev_io_stat, so seastar application does not need to have
    /// access spdk header files to compile.
    struct io_stats {
        uint64_t bytes_read;
        uint64_t num_read_ops;
        uint64_t bytes_written;
        uint64_t num_write_ops;
        uint64_t bytes_unmapped;
        uint64_t num_unmap_ops;
        uint64_t read_latency_ticks;
        uint64_t write_latency_ticks;
        uint64_t unmap_latency_ticks;
        uint64_t ticks_rate;
    };
public:
    static std::unique_ptr<block_device> open(const std::string& bdev_name);
    ~block_device();

    future<> write(uint64_t pos, const void* buffer, size_t len);
    future<> writev(uint64_t pos, std::vector<iovec> iov);
    future<> read(uint64_t pos, void* buffer, size_t len);
    future<> readv(uint64_t pos, std::vector<iovec> iov);
    future<> flush(uint64_t pos, size_t len);
    future<> unmap(uint64_t pos, size_t len);
    future<io_stats> stat();

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

class dev_iterator : public std::iterator<std::forward_iterator_tag, std::string> {
    void advance() noexcept;
public:
    dev_iterator(spdk_bdev* bdev, bool with_vbdev)
        : _bdev{bdev}
        , _with_vbdev{with_vbdev}
    {}
    dev_iterator& operator++() noexcept {
        advance();
        return *this;
    }
    dev_iterator operator++(int) noexcept {
        auto ret = *this;
        advance();
        return ret;
    }
    std::string operator*() const noexcept;
    bool operator==(const dev_iterator& other) const noexcept {
        return _bdev == other._bdev;
    }
private:
    spdk_bdev* _bdev;
    const bool _with_vbdev;
};

class list_devices {
public:
    explicit list_devices(bool with_vbdev)
        : _with_vbdev{with_vbdev}
    {}
    dev_iterator begin() const;
    dev_iterator end() const;
private:
    const bool _with_vbdev;
};

}

#endif // SEASTAR_HAVE_SPDK
