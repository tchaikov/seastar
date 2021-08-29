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

#include <boost/intrusive/list.hpp>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <memory>

struct spdk_thread;

namespace seastar::spdk {

namespace internal {

namespace bi = boost::intrusive;

class thread_entry {
    bi::list_member_hook<> _hook;
public:
    using container_list_t = bi::list<thread_entry,
        bi::member_hook<thread_entry, bi::list_member_hook<>, &thread_entry::_hook>>;
    spdk_thread* thread() noexcept;
    static thread_entry* from_thread(spdk_thread* thread);
};

class thread_msg {
public:
    virtual void run_and_dispose() noexcept = 0;
    seastar::future<> get_future() {
        return _pr.get_future();
    }
protected:
    seastar::promise<> _pr;
    ~thread_msg() = default;
};

template <typename Func>
class lambda_thread_msg final : public thread_msg {
    Func _func;
public:
    lambda_thread_msg(Func&& func) : _func(std::move(func)) {}
    void run_and_dispose() noexcept final {
        std::move(_func)();
        _pr.set_value();
        delete this;
    }
};
}

/// An executor of a group of SPDK threads.
///
/// \c spdk_thread is a user-space lightweight thread. SPDK uses it to
/// perform tasks on demand and to poll for events. In general, developer
/// should use \c seastar::spdk::app instead for setting up the SPDK
/// environment, but \c seastar::spdk::executor is also exposed when
/// a full-blown \c seastar::spdk::app is not necessary.
class executor : public peering_sharded_service<executor> {
    using sharded_executor_t = sharded<executor>;
public:
    future<> start();
    future<> stop();
    bool poll();
    void schedule_thread(spdk_thread* thread);

    /// run the specified function on a SPDK thread
    template <typename Func>
    static future<> submit_to(spdk_thread *thread, Func&& func) noexcept {
        auto msg = new internal::lambda_thread_msg<Func>(std::move(func));
        return do_submit_to(thread, msg);
    }
    /// returns an singleton of executor which is used for scheduling a new
    /// thread
    static sharded_executor_t& instance();

private:
    static future<> do_submit_to(spdk_thread* thread, internal::thread_msg* msg);
    std::unique_ptr<reactor::poller> poller;
    internal::thread_entry::container_list_t _threads;
    uint64_t _tsc_last;
    static sharded_executor_t* s_executor;
};

}

#endif // SEASTAR_HAVE_SPDK
