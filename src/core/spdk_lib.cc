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

#include <seastar/core/spdk_lib.hh>
#include <spdk/env.h>

namespace seastar::spdk {

temporary_buffer<char> dma_zmalloc(size_t size, size_t align)
{
    void* buf = spdk_dma_zmalloc_socket(size, align, nullptr, SPDK_ENV_SOCKET_ID_ANY);
    if (!buf) {
        throw std::bad_alloc();
    }
    return {static_cast<char*>(buf), size, seastar::make_deleter([buf] {
        spdk_dma_free(buf);
    })};
}

}
