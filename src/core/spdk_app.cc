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

#include <seastar/core/spdk_app.hh>
#include <seastar/util/defer.hh>
#include <seastar/core/thread.hh>
#include <spdk/cpuset.h>
#include <spdk/env.h>
#include <spdk/init.h>
#include <spdk/log.h>
#include <spdk/string.h>
#include <spdk/thread.h>

namespace seastar::spdk {

seastar::logger logger("spdk");

namespace env {

void start(const std::vector<resource::cpu>& cpuset,
           const boost::program_options::variables_map& opts)
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

    if (opts.count("spdk-mem-size")) {
        const std::string mem_size_str = opts["spdk-mem-size"].as<std::string>();
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
    if (opts.count("spdk-huge-dir")) {
        hugedir = opts["spdk-huge-dir"].as<std::string>();
        env_opts.hugedir = hugedir.c_str();
    }
    if (opts.count("spdk-single-file-segments")) {
        env_opts.hugepage_single_segments = true;
    }
    if (opts.count("spdk-huge-unlink")) {
        env_opts.unlink_hugepage = true;
    }
    if (opts.count("spdk-no-pci")) {
        env_opts.no_pci = true;
    }
    std::vector<spdk_pci_addr> pci_addrs;
    if (opts.count("spdk-pci-blocked") && opts.count("spdk-pci-allowed")) {
        throw std::invalid_argument("--spdk-pci-blocked and --spdk-pci-allowed cannot be used at the same time");
    } else if (opts.count("spdk-pci-blocked")) {
        for (const auto& bdf : opts["spdk-pci-blocked"].as<std::vector<std::string>>()) {
            spdk_pci_addr pci_addr;
            spdk_pci_addr_parse(&pci_addr, bdf.c_str());
            pci_addrs.push_back(pci_addr);
        }
        env_opts.pci_blocked = &pci_addrs[0];
        env_opts.num_pci_addr = pci_addrs.size();
    } else if (opts.count("spdk-pci-allowed")) {
        for (const auto& bdf : opts["spdk-pci-allowed"].as<std::vector<std::string>>()) {
            spdk_pci_addr pci_addr;
            spdk_pci_addr_parse(&pci_addr, bdf.c_str());
            pci_addrs.push_back(pci_addr);
        }
        env_opts.pci_allowed = &pci_addrs[0];
        env_opts.num_pci_addr = pci_addrs.size();
    }
    std::string iova_mode;
    if (opts.count("spdk-iova-mode")) {
        iova_mode = opts["spdk-iova-mode"].as<std::string>();
        env_opts.iova_mode = iova_mode.c_str();
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
        seastar::spdk::logger.info("subsystem initialized: {}", rc);
        if (rc) {
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

}

namespace seastar::spdk {

future<int> app::run(const boost::program_options::variables_map& opts,
                  std::function<future<> ()>&& func) noexcept
{
    if (!opts.count("spdk-pmd")) {
        std::cerr << "SPDK app cannot start without --spdk-pmd enabled!" << std::endl;
        return make_ready_future<int>(1);
    } 
    spdk_log_open(spdk_do_log);

    return seastar::async([opts, func = std::move(func), this] {
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

future<> app::start(const boost::program_options::variables_map& opts)
{
    logger.info("app start");
    // ensure that start() is able to find app_thread using spdk_get_thread(),
    // the underlying SPDK functions need to hook poolers to "this" thread.
    auto init_desc = std::make_unique<subsystem_init_desc>();
    auto init_done = init_desc->get_future();
    auto rpc_addr = opts["spdk-rpc-socket"].as<std::string>();
    if (opts.count("spdk-config")) {

        auto spdk_config = opts["spdk-config"].as<std::string>();
        spdk_subsystem_init_from_json_config(
            spdk_config.c_str(),
            rpc_addr.c_str(),
            spdk_subsystem_init_cpl,
            init_desc.release(),
            opts.count("spdk-json-ignore-init-errors"));
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
    auto fini_desc = std::make_unique<msg_desc>();
    auto fini_done = fini_desc->get_future();
    spdk_subsystem_fini(spdk_subsystem_fini_cpl, fini_desc.release());
    return fini_done;
}

boost::program_options::options_description app::get_options_description()
{
    namespace bpo = boost::program_options;
    bpo::options_description opts("SPDK options");
    opts.add_options()
        ("spdk-rpc-socket",
         bpo::value<std::string>()->default_value(SPDK_DEFAULT_RPC_ADDR),
         "RPC listen address")
        ("spdk-config", bpo::value<std::string>(), "JSON config file")
        ("spdk-json-ignore-init-errors", "don't exit on invalid config entry")
        ("spdk-iova-mode", bpo::value<std::string>(),
         "set IOVA mode ('pa' for IOVA_PA and 'va' for IOVA_VA)")
        ("spdk-huge-dir", bpo::value<std::string>(),
         "use a specific hugetlbfs mount to reserve memory from")
        ("spdk-huge-unlink", "unlink huge files after initialization")
        ("spdk-mem-size", bpo::value<std::string>(),
         "memory size in MB for DPDK")
        ("spdk-no-pci", "disable PCI access")
        ("spdk-single-file-segments", "force creating just one hugetlbfs file")
        ("spdk-pci-blocked", bpo::value<std::vector<std::string>>()->multitoken(),
         "pci addr to block (can be used more than once)")
        ("spdk-pci-allowed", bpo::value<std::vector<std::string>>()->multitoken(),
         "pci addr to allow (--spdk-pci-blocked and --spdk-pci-allowed cannot be used at the same time)")
    ;
    return opts;
}

}
