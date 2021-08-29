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
 * Copyright (C) 2022 Kefu Chai <tchaikov@gmail.com>
 */

#pragma once

#ifdef SEASTAR_HAVE_SPDK

#include <seastar/util/program-options.hh>

namespace seastar::spdk {

enum class iova_mode {
    pa,
    va,
};

/// SPDK configuration options.
struct options : program_options::option_group {
    program_options::value<std::string> rpc_addr;
    program_options::value<std::string> json_config;
    program_options::value<> json_ignore_init_errors;
    program_options::selection_value<iova_mode> iova;
    program_options::value<std::string> huge_dir;
    program_options::value<> huge_unlink;
    program_options::value<std::string> mem_size;
    program_options::value<> no_pci;
    program_options::value<> single_file_segments;
    options(program_options::option_group* parent_group);
};

}

namespace fmt {

template <> struct formatter<seastar::spdk::iova_mode>: formatter<std::string_view> {
    template <typename FormatContext>
    auto format(seastar::spdk::iova_mode mode, FormatContext& ctx) const {
        string_view name = "unknown";
        switch (mode) {
            case seastar::spdk::iova_mode::pa:
                name = "pa";
                break;
            case seastar::spdk::iova_mode::va:
                name = "va";
                break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

}

#endif // SEASTAR_HAVE_SPDK
