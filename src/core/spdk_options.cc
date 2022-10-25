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

#include <seastar/core/spdk_options.hh>
#include <spdk/init.h>
#include <spdk/trace.h>

namespace seastar::spdk {

static auto iova_modes()
-> program_options::selection_value<iova_mode>::candidates {

    auto iova_value = [](auto mode, auto name) {
        return program_options::selection_value<iova_mode>::candidate{
        name,
        {new iova_mode(mode), std::default_delete<iova_mode>()},
        std::unique_ptr<program_options::option_group>{}};
    };

    program_options::selection_value<iova_mode>::candidates candidates;
    candidates.push_back(iova_value(iova_mode::va, "va"));
    candidates.push_back(iova_value(iova_mode::pa, "pa"));
    return candidates;
}

options::options(program_options::option_group* parent_group)
    : program_options::option_group(parent_group, "SPDK options")
    , name(*this, "spdk-name", "spdk", "RPC listen address")
    , rpc_addr(*this, "spdk-rpc-socket", SPDK_DEFAULT_RPC_ADDR, "RPC listen address")
    , json_config(*this, "spdk-config", {}, "JSON config file")
    , json_ignore_init_errors(*this, "spdk-json-ignore-init-errors", "don't exit on invalid config entry")
    , iova(*this, "spdk-iova-mode", iova_modes(), "set IOVA mode ('pa' for IOVA_PA and 'va' for IOVA_VA)")
    , huge_dir(*this, "spdk-huge-dir", {}, "use a specific hugetlbfs mount to reserve memory from")
    , huge_unlink(*this, "spdk-huge-unlink", "unlink huge files after initialization")
    , mem_size(*this, "spdk-mem-size", {}, "memory size in MB for DPDK")
    , no_pci(*this, "spdk-no-pci", "disable PCI access")
    , single_file_segments(*this, "spdk-single-file-segments", "force creating just one hugetlbfs file")
    , tracepoint_entries(*this, "spdk-tracepoint-entries", SPDK_DEFAULT_NUM_TRACE_ENTRIES, "number of tracepoint entries preserved in ringbuffer")
    , tracepoint_masks(*this, "spdk-tracepoint-masks", {}, "tracepoint masks for spdk trace buffers")
    , env_context(*this, "spdk-env-context", {}, "Opaque context for use of the env implementation")
{
}

}

#endif
