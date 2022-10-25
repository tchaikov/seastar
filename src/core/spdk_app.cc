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

#include <bit>
#include <bitset>
#include <charconv>
#include <ranges>

#include <seastar/core/spdk_app.hh>
#include <seastar/core/spdk_options.hh>
#include <seastar/util/defer.hh>
#include <seastar/core/thread.hh>
#include <spdk/cpuset.h>
#include <spdk/env.h>
#include <spdk/init.h>
#include <spdk/log.h>
#include <spdk/string.h>
#include <spdk/thread.h>
#include <spdk/trace.h>

namespace seastar::spdk {

seastar::logger logger("spdk");

namespace env {

void start(const std::vector<resource::cpu>& cpuset,
           const options& opts)
{
    logger.info("env starting");
    spdk_env_opts env_opts = {};
    spdk_env_opts_init(&env_opts);

    std::string core_list;
    for (auto& cpu : cpuset) {
        if (!core_list.empty()) {
            core_list.append(",");
        }
        core_list.append(std::to_string(cpu.cpu_id));
    }
    core_list = fmt::format("[{}]", core_list);
    env_opts.core_mask = core_list.c_str();

    if (opts.mem_size) {
        const std::string mem_size_str = opts.mem_size.get_value();
        uint64_t mem_size_mb;
        bool mem_size_has_prefix;
        if (spdk_parse_capacity(mem_size_str.c_str(),
                                &mem_size_mb,
                                &mem_size_has_prefix) != 0) {
            throw std::invalid_argument(
                fmt::format("invalid memory pool size `--mem-size {}`",
                            mem_size_str));
        }
        if (mem_size_has_prefix) {
            // convert mem size to MiB
            mem_size_mb >>= 20;
        }
        if (mem_size_mb > std::numeric_limits<int>::max()) {
            throw std::invalid_argument(
                fmt::format("memory pool size too large `--mem-size {}`",
                            mem_size_mb));
        }
        env_opts.mem_size = static_cast<int>(mem_size_mb);
    }
    std::string hugedir;
    if (opts.huge_dir) {
        hugedir = opts.huge_dir.get_value();
        env_opts.hugedir = hugedir.c_str();
    }
    if (opts.single_file_segments) {
        env_opts.hugepage_single_segments = true;
    }
    if (opts.huge_unlink) {
        env_opts.unlink_hugepage = true;
    }
    if (opts.no_pci) {
        env_opts.no_pci = true;
    }
    if (opts.iova) {
        env_opts.iova_mode = opts.iova.get_selected_candidate_name().c_str();
    }
    std::string env_context;
    if (opts.env_context) {
        env_context = opts.env_context.get_value();
        env_opts.env_context = reinterpret_cast<void*>(env_context.data());
    }
    if (spdk_env_init(&env_opts) < 0) {
        throw std::runtime_error("unable to initialize SPDK env");
    }
    logger.info("env starting: done");
}

void stop() noexcept
{
    logger.info("env stopping");
    spdk_env_fini();
}
}
}

namespace {
class subsystem_init_desc {
    seastar::promise<> _pr;
public:
    void complete_with(int rc) {
        if (rc) {
            seastar::spdk::logger.error("unable to initialize subsystem: {}", spdk_strerror(-rc));
            _pr.set_exception(std::runtime_error("unable to init SPDK subsystem"));
        } else {
            _pr.set_value();
        }
        delete this;
    }
    seastar::future<> get_future() {
        return _pr.get_future();
    }
};

class msg_desc {
    seastar::promise<> _pr;
public:
    void complete() {
        _pr.set_value();
        delete this;
    }
    seastar::future<> get_future() {
        return _pr.get_future();
    }
};

constexpr seastar::log_level spdk_log_to_seastar_level(int level)
{
    switch (level) {
    case SPDK_LOG_DISABLED:
        return seastar::log_level(static_cast<int>(seastar::log_level::trace) + 1);
    case SPDK_LOG_ERROR:
        return seastar::log_level::error;
    case SPDK_LOG_WARN:
        return seastar::log_level::warn;
    case SPDK_LOG_NOTICE:
        return seastar::log_level::info;
    case SPDK_LOG_INFO:
        return seastar::log_level::debug;
    case SPDK_LOG_DEBUG:
        return seastar::log_level::trace;
    default:
        return seastar::log_level::info;
    }
}

void spdk_do_log(int level, const char *file, const int line,
              const char *func, const char *format, va_list args)
{
    static const int MAX_TMPBUF = 1024;
    char buf[MAX_TMPBUF];
    int len = vsnprintf(buf, sizeof(buf), format, args);
    if (len > 0 && buf[len - 1] == '\n') {
        // remove the trailing newline, as seastar always add it for us
        buf[len - 1] = '\0';
    }
    seastar::spdk::logger.log(spdk_log_to_seastar_level(level),
                              "{}:{:4d}:{}: {}",
                              file, line, func, buf);
}

// SPDK keeps track of the "current" spdk_thread using a thread local storage
// variable. and it uses a dedicated spdk_thread (app_thread) for setting up
// SPDK app environment after the reactors are up and running. but seastar::spdk::app
// is a little bit different, it also spawns an "app_thread". but it does not
// schedule the setting up task using spdk_thread_send_msg() call, which runs
// the task when the spdk thread is scheduled. instead, seastar::spdk::app
// schedule the setup tasks using Seastar primitives directly. To ensure that
// the tasks have access to the "current" spdk_thread, we have to set the
// TLS variable manually. run_with_spdk_thread is defined to do this job.
struct run_with_spdk_thread {
    run_with_spdk_thread(spdk_thread* thread) {
        spdk_set_thread(thread);
    }
    ~run_with_spdk_thread() {
        spdk_set_thread(nullptr);
    }
};

int setup_trace(std::string_view name,
                 std::string_view group_masks,
                 uint64_t num_entries)
{
    // setup the ringbuffer shm for holding the tracepoints
    if (num_entries == 0) {
        return 0;
    }
    if (!std::has_single_bit(num_entries)) {
        seastar::spdk::logger.error("tracepoint-entries must be power of 2");
        return 1;
    }
    // mimic the behavior of an SPDK app, see also
    // spdk/lib/event/app.c: app_setup_trace(struct spdk_app_opts *opts)
    std::string shm_name = fmt::format("{}_trace.pid{}", name, static_cast<int>(getpid()));
    if (int rc = spdk_trace_init(shm_name.c_str(), num_entries); rc != 0) {
        seastar::spdk::logger.error("unable to init trace buffer {}", shm_name);
        return rc;
    }
    // parse a comma-separaterd tracepoint group and its optional mask, like
    // "nvmf_tcp:0x1,thread", please note, unlike spdk, we don't support the notation like
    // "nvmf_tcp,thread:0x1"
    for (const auto gm : std::views::split(group_masks, ",")) {
        std::string_view group_and_mask{gm.begin(), gm.end()};
        std::string group;
        uint64_t per_group_mask = 0;
        if (auto mask_pos = group_and_mask.find(':'); mask_pos != group_and_mask.npos) {
            group = group_and_mask.substr(0, mask_pos);
            auto mask_str = group_and_mask.substr(mask_pos + 1);
            auto [ptr, ec] = std::from_chars(mask_str.data(),
                                             mask_str.data() + mask_str.size(),
                                             per_group_mask, 16);
            if (ec != std::errc() || ptr != mask_str.data() + mask_str.size()) {
                seastar::spdk::logger.warn("unrecognized tracepoint mask: {} for {}",
                                           mask_str, group);
                return 1;
            }
        } else {
            group.assign(group_and_mask);
            per_group_mask = std::numeric_limits<uint64_t>::max();
        }
        // please note, group could contain more than one group. for instance "all" implies that
        // all tracepoint groups should be enabled. so we need to set all
        uint64_t group_mask = spdk_trace_create_tpoint_group_mask(group.c_str());
        if (group_mask == 0) {
            seastar::spdk::logger.warn("unrecognized tracepoint group: {}", group);
            continue;
        }
        std::bitset<SPDK_TRACE_MAX_GROUP_ID> enabled_groups(group_mask);
        for (std::size_t group_id = 0; group_id < enabled_groups.size(); group_id++) {
            if (enabled_groups.test(group_id)) {
                seastar::spdk::logger.debug("tracepoint {} ({}) enabled with {:#x}",
                                            group, group_id, per_group_mask);
                spdk_trace_set_tpoints(group_id, per_group_mask);
            }
        }
    }
    seastar::spdk::logger.info("tracepoint located at /dev/shm/{}", shm_name);
    seastar::spdk::logger.info("use 'spdk_trace -s {} -p {}' to capture a snapshot of events at runtime",
                               name, getpid());
    return 0;
}

}

namespace seastar::spdk {

future<int> app::run(const options& opts,
                     std::function<future<> ()>&& func) noexcept
{
    return run(opts, [func = std::move(func)] {
        return func().then([] {
            return 0;
        });
    });
}

future<int> app::run(const options& opts,
                     std::function<future<int> ()>&& func) noexcept
{
    spdk_log_open(spdk_do_log);

    return seastar::async([&opts, func = std::move(func), this] {
        sharded_executor.start().then([this] {
            return sharded_executor.invoke_on_all(&executor::start);
        }).get();
        auto stop_executor = seastar::defer([&] () noexcept {
            sharded_executor.stop().get();
        });
        assert(app_thread == nullptr);
        spdk_cpuset cpu_mask = {};
        spdk_cpuset_set_cpu(&cpu_mask, spdk_env_get_current_core(), true);
        app_thread = spdk_thread_create("app_thread", &cpu_mask);
        if (app_thread == nullptr) {
            throw std::bad_alloc();
        }
        if (opts.tracepoint_masks) {
            if (setup_trace(opts.name.get_value(),
                            opts.tracepoint_masks.get_value(),
                            opts.tracepoint_entries.get_value())) {
                return 1;
            }
        }
        run_with_spdk_thread run_with(app_thread);
        start(opts).get();
        auto stop_me = seastar::defer([&] () noexcept {
            stop().get();
        });

        try {
            futurize_invoke(func).get();
            return 0;
        } catch (std::exception& e) {
            std::cerr << e.what() << std::endl;
            return 1;
        } catch (...) {
            std::cerr << "unknown exception" << std::endl;
            return 1;
        }
    });
}

static void spdk_subsystem_init_cpl(int rc, void* arg)
{
    auto* desc = static_cast<subsystem_init_desc*>(arg);
    desc->complete_with(rc);
}

future<> app::start(const options& opts)
{
    logger.info("app start");
    // ensure that start() is able to find app_thread using spdk_get_thread(),
    // the underlying SPDK functions need to hook poolers to "this" thread.
    auto init_desc = std::make_unique<subsystem_init_desc>();
    auto init_done = init_desc->get_future();
    auto rpc_addr = opts.rpc_addr.get_value();
    if (opts.json_config) {
        spdk_subsystem_init_from_json_config(
            opts.json_config.get_value().c_str(),
            rpc_addr.c_str(),
            spdk_subsystem_init_cpl,
            init_desc.release(),
            opts.json_ignore_init_errors);
    } else {
        spdk_subsystem_init(
            spdk_subsystem_init_cpl,
            init_desc.release());
    }
    return init_done.then([rpc_addr] {
        if (int rc = spdk_rpc_initialize(rpc_addr.c_str()); rc != 0) {
            throw std::runtime_error("unable to init SPDK RPC");
        }
    });
}

// seastar takes care of the cleanup, so just use a dummy callback here
static void spdk_subsystem_fini_cpl(void* arg)
{
    auto *desc = static_cast<msg_desc*>(arg);
    desc->complete();
}

future<> app::stop()
{
    logger.info("app stopping");
    spdk_rpc_finish();
    spdk_trace_cleanup();
    auto fini_desc = std::make_unique<msg_desc>();
    auto fini_done = fini_desc->get_future();
    spdk_subsystem_fini(spdk_subsystem_fini_cpl, fini_desc.release());
    return fini_done;
}

}

#endif // SEASTAR_HAVE_SPDK
