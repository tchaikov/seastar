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

#include <seastar/util/log.hh>
#include <seastar/core/spdk_thread.hh>
#include <spdk/thread.h>

namespace seastar::spdk {

extern logger logger;

spdk_thread* thread_entry::thread() noexcept
{
    return spdk_thread_get_from_ctx(reinterpret_cast<void*>(this));
}

thread_entry* thread_entry::from_thread(spdk_thread* thread)
{
    return static_cast<thread_entry*>(spdk_thread_get_ctx(thread));
}

static int thread_do_op(spdk_thread* thread, spdk_thread_op op)
{
    switch (op) {
    case SPDK_THREAD_OP_NEW: {
        spdk_cpuset* cpumask = spdk_thread_get_cpumask(thread);
        unsigned shard = 0;
        while (shard < smp::count) {
            if (spdk_cpuset_get_cpu(cpumask, shard)) {
                break;
            }
        }
        if (shard == smp::count) {
            logger.error("unable to find executor for new thread");
            return -1;
        }
        // FIXME: future is discarded
        (void)executor::instance().invoke_on(
            shard, [thread] (executor& group) {
                group.schedule_thread(thread);
            });
        return 0;
    }
    case SPDK_THREAD_OP_RESCHED:
        return -ENOTSUP;
    default:
        return -ENOTSUP;
    }
}

static bool thread_op_supported(spdk_thread_op op)
{
    switch (op) {
    case SPDK_THREAD_OP_NEW:
        return true;
    case SPDK_THREAD_OP_RESCHED:
        return false;
    default:
        return false;
    }
}

future<> executor::start()
{
    logger.info("executor#{} start", seastar::this_shard_id());
    poller = std::make_unique<reactor::poller>(reactor::poller::simple([this] {
      return poll();
    }));
    if (seastar::this_shard_id() == 0) {
        spdk_thread_lib_init_ext(thread_do_op, thread_op_supported,
                                 sizeof(thread_entry));
        sharded_executor_t& instance = container();
        s_executor = &instance;
    }
    return make_ready_future<>();
}

future<> executor::stop()
{
    if (seastar::this_shard_id() == 0) {
        s_executor = nullptr;
        spdk_thread_lib_fini();
    }
    poller.reset();
    return make_ready_future<>();
}

bool executor::poll()
{
    int nr = 0;
    for (auto& entry : _threads) {
        spdk_thread *thread = entry.thread();
        nr += spdk_thread_poll(thread, 0, _tsc_last);
        _tsc_last = spdk_thread_get_last_tsc(thread);
        if (__builtin_expect(spdk_thread_is_exited(thread) &&
                             spdk_thread_is_idle(thread), false)) {
            _threads.erase(thread_entry::container_list_t::s_iterator_to(entry));
            spdk_thread_destroy(thread);
        }
    }
    logger.trace("poll(): {}", nr);
    return nr > 0;
}

void executor::schedule_thread(spdk_thread* thread)
{
    _threads.push_back(*thread_entry::from_thread(thread));
}

executor::sharded_executor_t& executor::instance()
{
    assert(s_executor);
    return *s_executor;
}

static void spdk_msg_call(void* ctx)
{
    auto* task = static_cast<internal::thread_msg*>(ctx);
    task->run_and_dispose();
}

future<> executor::do_send_to(spdk_thread* thread,
                              internal::thread_msg* msg)
{
    spdk_thread_send_msg(thread, spdk_msg_call, msg);
    return msg->get_future();
}

executor::sharded_executor_t* executor::s_executor = nullptr;

run_with_spdk_thread::run_with_spdk_thread(spdk_thread* thread) {
    spdk_set_thread(thread);
}

run_with_spdk_thread::~run_with_spdk_thread() {
    spdk_set_thread(nullptr);
}

}
