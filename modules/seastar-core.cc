// -*- mode:C++; tab-width:4; c-basic-offset:4; indent-tabs-mode:nil -*-
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
 * Copyright 2023 ScyllaDB
 */
module;

#include <seastar/core/abort_on_ebadf.hh>
#include <seastar/core/abort_on_expiry.hh>
#include <seastar/core/abort_source.hh>
#include <seastar/core/alien.hh>
#include <seastar/core/align.hh>
#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/array_map.hh>
#include <seastar/core/bitops.hh>
#include <seastar/core/bitset-iter.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/checked_ptr.hh>
#include <seastar/core/chunked_fifo.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/circular_buffer_fixed_capacity.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/deleter.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/enum.hh>
#include <seastar/core/execution_stage.hh>
#include <seastar/core/expiring_fifo.hh>
#include <seastar/core/execution_stage.hh>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/file.hh>
#include <seastar/core/file-types.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/make_task.hh>
#include <seastar/core/manual_clock.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/manual_clock.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/metrics_types.hh>
#include <seastar/core/on_internal_error.hh>
#include <seastar/core/pipe.hh>
#include <seastar/core/polymorphic_temporary_buffer.hh>
#include <seastar/core/preempt.hh>
#include <seastar/core/prefetch.hh>
#include <seastar/core/print.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/ragel.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/rwlock.hh>
#include <seastar/core/scattered_message.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/scheduling_specific.hh>
#include <seastar/core/scollectd.hh>
#include <seastar/core/scollectd_api.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/shared_mutex.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/simple-stream.hh>
#include <seastar/core/slab.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/smp_options.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/stream.hh>
#include <seastar/core/task.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/timed_out_error.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/timer-set.hh>
#include <seastar/core/unaligned.hh>
#include <seastar/core/units.hh>
#include <seastar/core/vector-data-sink.hh>
#include <seastar/core/weak_ptr.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/when_any.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/with_timeout.hh>

export module seastar:core;

// abort_on_ebadf
export namespace seastar {
    using ::seastar::set_abort_on_ebadf;
    using ::seastar::is_abort_on_ebadf_enabled;
}
// abort_on_expiry
export namespace seastar {
    using ::seastar::abort_on_expiry;
}
// abort_source
export namespace seastar {
    using ::seastar::abort_requested_exception;
    using ::seastar::abort_source;
}
// alien
export namespace seastar::alien {
    using ::seastar::alien::message_queue;
    using ::seastar::alien::instance;
    using ::seastar::alien::run_on;
    using ::seastar::alien::submit_to;
}
// align
export namespace seastar {
    using ::seastar::align_up;
    using ::seastar::align_down;
}
// aligned_buffer
export namespace seastar {
    using ::seastar::free_deleter;
    using ::seastar::allocate_aligned_buffer;
}
// aligned_buffer
export namespace seastar {
    using ::seastar::free_deleter;
    using ::seastar::allocate_aligned_buffer;
}
// app-template
export namespace seastar {
    using ::seastar::app_template;
}
// array_map
export namespace seastar {
    using ::seastar::array_map;
}
// bitops
export namespace seastar {
    using ::seastar::count_leading_zeros;
    using ::seastar::count_trailing_zeros;
    using ::seastar::log2ceil;
    using ::seastar::log2floor;
}
// bitset-iter
export namespace seastar::bitsets {
    using ::seastar::bitsets::set_range;
    // TODO: bitsets::for_each_set has internal linkage, so we cannot export it this way
}
// byteorder
export namespace seastar {
    using ::seastar::cpu_to_le;
    using ::seastar::le_to_cpu;
    using ::seastar::cpu_to_be;
    using ::seastar::be_to_cpu;
    using ::seastar::read_le;
    using ::seastar::write_le;
    using ::seastar::read_be;
    using ::seastar::write_be;
    using ::seastar::consume_be;
    using ::seastar::produce_be;
}
// cacheline
export namespace seastar {
    // TODO: cache_line_size has internal linkage, so we cannot export it this way
}
// checked_ptr
export namespace seastar {
    using ::seastar::checked_ptr_is_null_exception;
    using ::seastar::default_null_deref_action;
    using ::seastar::checked_ptr;
}
// chunked_fifo
export namespace seastar {
    using ::seastar::chunked_fifo;
}
// circular_buffer
export namespace seastar {
    using ::seastar::circular_buffer;
}
// circular_buffer_fixed_capacity
export namespace seastar {
    using ::seastar::circular_buffer_fixed_capacity;
}
// condition-variable
export namespace seastar {
    using ::seastar::broken_condition_variable;
    using ::seastar::condition_variable_timed_out;
    using ::seastar::condition_variable;
}
// coroutine
export namespace seastar {
    using ::seastar::operator co_await;
}
export namespace seastar::coroutine {
    using ::seastar::coroutine::without_preemption_check;
    using ::seastar::coroutine::lambda;
}
// deleter
export namespace seastar {
    using ::seastar::deleter;
    using ::seastar::make_free_deleter;
    using ::seastar::make_object_deleter;
}
// coroutine
export namespace seastar {
    using ::seastar::operator co_await;
}
// do_with
export namespace seastar {
    using ::seastar::do_with;
    using ::seastar::with_lock;
    using ::seastar::operator co_await;
    using ::seastar::operator co_await;
    using ::seastar::operator co_await;
}
// enum
export namespace seastar {
    using ::seastar::enum_hash;
}
// execution_stage
export namespace seastar {
    using ::seastar::execution_stage;
    using ::seastar::concrete_execution_stage;
    using ::seastar::inheriting_execution_stage;
    using ::seastar::inheriting_concrete_execution_stage;
    using ::seastar::make_execution_stage;
}
// expiring_fifo
export namespace seastar {
    using ::seastar::dummy_expiry;
    using ::seastar::promise_expiry;
    using ::seastar::expiring_fifo;
}
// fair_queue
export namespace seastar {
    using ::seastar::fair_queue_ticket;
    using ::seastar::fair_queue_entry;
    using ::seastar::fair_group;
    using ::seastar::fair_queue;
}
// file
export namespace seastar {
    using ::seastar::directory_entry;
    using ::seastar::stat_data;
    using ::seastar::file_open_options;
    using ::seastar::file;
    using ::seastar::with_file;
    using ::seastar::with_file_close_on_failure;
    using ::seastar::file_handle;
    using ::seastar::cancelled_error;
}
// file-types
export namespace seastar {
    using ::seastar::open_flags;
    using ::seastar::operator|;
    using ::seastar::operator|=;
    using ::seastar::operator&;
    using ::seastar::operator&=;
    using ::seastar::directory_entry_type;
    using ::seastar::fs_type;
    using ::seastar::access_flags;
    using ::seastar::file_permissions;
}
// fsteam
export namespace seastar {
    using ::seastar::file_input_stream_history;
    using ::seastar::file_input_stream_options;
    using ::seastar::make_file_input_stream;
    using ::seastar::file_output_stream_options;
}
// future
export namespace seastar {
    using ::seastar::make_ready_future;
    using ::seastar::make_exception_future;
    using ::seastar::broken_promise;
    using ::seastar::current_exception_as_future;
    using ::seastar::continuation;
    using ::seastar::futurize_invoke;
    using ::seastar::futurize_apply;
    using ::seastar::future;
    using ::seastar::futurize;
}
// gate
export namespace seastar {
    using ::seastar::gate_closed_exception;
    using ::seastar::gate;
    using ::seastar::with_gate;
    using ::seastar::try_with_gate;
}
// iostream
export namespace seastar {
    using ::seastar::data_sink;
    using ::seastar::continue_consuming;
    using ::seastar::stop_consuming;
    using ::seastar::skip_bytes;
    using ::seastar::consumption_result;
    using ::seastar::input_stream;
    using ::seastar::output_stream_options;
    using ::seastar::output_stream;
    using ::seastar::copy;
}
// loop
export namespace seastar {
    using ::seastar::stop_iteration;
    using ::seastar::repeat;
    using ::seastar::repeat_until_value;
    using ::seastar::do_until;
    using ::seastar::keep_doing;
    using ::seastar::do_for_each;
    using ::seastar::parallel_for_each;
    using ::seastar::max_concurrent_for_each;
}
// lowres_clock
export namespace seastar {
    using ::seastar::lowres_clock;
    using ::seastar::lowres_system_clock;
}
// make_task
export namespace seastar {
    using ::seastar::make_task;
}
// manual_clock
export namespace seastar {
    using ::seastar::manual_clock;
}
// map_reduce
export namespace seastar {
    using ::seastar::map_reduce;
    using ::seastar::adder;
}
// memory
export namespace seastar::memory {
    using ::seastar::memory::stats;
    using ::seastar::memory::statistics;
    using ::seastar::memory::memory_layout;
    using ::seastar::memory::get_memory_layout;
    using ::seastar::memory::free_memory;
    using ::seastar::memory::min_free_memory;
    using ::seastar::memory::set_min_free_pages;
    using ::seastar::memory::set_large_allocation_warning_threshold;
    using ::seastar::memory::get_large_allocation_warning_threshold;
    using ::seastar::memory::disable_large_allocation_warning;
    using ::seastar::memory::scoped_large_allocation_warning_threshold;
    using ::seastar::memory::scoped_large_allocation_warning_disable;
    using ::seastar::memory::set_heap_profiling_enabled;
    using ::seastar::memory::scoped_heap_profiling;
}
// metrics
export namespace seastar::metrics {
    using ::seastar::metrics::double_registration;
    using ::seastar::metrics::description;
    using ::seastar::metrics::label_instance;
    using ::seastar::metrics::label;
    using ::seastar::metrics::metric_disabled;
    using ::seastar::metrics::shard_label;
    using ::seastar::metrics::make_gauge;
    using ::seastar::metrics::make_derive;
    using ::seastar::metrics::make_counter;
    using ::seastar::metrics::make_absolute;
    using ::seastar::metrics::make_histogram;
    using ::seastar::metrics::make_summary;
    using ::seastar::metrics::make_total_bytes;
    using ::seastar::metrics::make_current_bytes;
    using ::seastar::metrics::make_queue_length;
    using ::seastar::metrics::make_total_operations;
}
// metrics_api
export namespace seastar::metrics {
    using ::seastar::metrics::options;
    using ::seastar::metrics::configure;
}
// metrics_registration
export namespace seastar::metrics {
    using ::seastar::metrics::metric_definition;
    using ::seastar::metrics::metric_group_definition;
    using ::seastar::metrics::metric_groups;
    using ::seastar::metrics::metric_group;
}
// metrics_types
export namespace seastar::metrics {
    using ::seastar::metrics::histogram_bucket;
    using ::seastar::metrics::histogram;
}
// on_internal_error
export namespace seastar {
    using ::seastar::set_abort_on_internal_error;
    using ::seastar::on_internal_error;
    using ::seastar::on_internal_error_noexcept;
    using ::seastar::on_fatal_internal_error;
}
// pipe
export namespace seastar {
    using ::seastar::broken_pipe_exception;
    using ::seastar::unread_overflow_exception;
    using ::seastar::pipe_reader;
    using ::seastar::pipe_writer;
    using ::seastar::pipe;
}
// polymorphic_temporary_buffer
export namespace seastar {
    using ::seastar::make_temporary_buffer;
}
// preempt
export namespace seastar {
    using ::seastar::need_preempt;
}
// prefetch
export namespace seastar {
    using ::seastar::prefetch_n;
    using ::seastar::prefetch;
    using ::seastar::prefetchw;
    using ::seastar::prefetchw_n;
}
// print
export namespace seastar {
    using ::seastar::fprint;
    using ::seastar::print;
    using ::seastar::sprint;
    using ::seastar::format_separated;
    using ::seastar::usecfmt;
    using ::seastar::log;
    using ::seastar::format;
}
// prometheus
export namespace seastar::prometheus {
    using ::seastar::prometheus::config;
    using ::seastar::prometheus::start;
    using ::seastar::prometheus::add_prometheus_routes;
}
// queue
export namespace seastar {
    using ::seastar::queue;
}
// ragel
export namespace seastar {
    using ::seastar::sstring_builder;
    using ::seastar::ragel_parser_base;
    using ::seastar::trim_trailing_spaces_and_tabs;
}
// reactor
export namespace seastar {
    using ::seastar::shard_id;
    using ::seastar::engine;
    using ::seastar::engine_is_ready;
}
// reactor_config
export namespace seastar {
    using ::seastar::reactor_options;
    using ::seastar::engine;
}
// resource
export namespace seastar {
    using ::seastar::cpuid_to_cpuset;
}
export namespace seastar::resource {
    using ::seastar::resource::configuration;
    using ::seastar::resource::memory;
    using ::seastar::resource::io_queue_topology;
    using ::seastar::resource::cpu;
    using ::seastar::resource::resources;
    using ::seastar::resource::io_queue_topology;
}
// rwlock
export namespace seastar {
    using ::seastar::rwlock;
}
// scattered_message
export namespace seastar {
    using ::seastar::scattered_message;
}
// scheduling
export namespace seastar {
    using ::seastar::max_scheduling_groups;
    using ::seastar::sched_clock;
    using ::seastar::create_scheduling_group;
    using ::seastar::destroy_scheduling_group;
    using ::seastar::rename_scheduling_group;
    using ::seastar::scheduling_group_key_config;
    using ::seastar::scheduling_group_key;
    using ::seastar::make_scheduling_group_key_config;
    using ::seastar::scheduling_group_key_create;
    using ::seastar::scheduling_group_get_specific;
    using ::seastar::scheduling_group;
    using ::seastar::current_scheduling_group;
    using ::seastar::default_scheduling_group;
    using ::seastar::default_scheduling_group;
}
// scheduling_specific
export namespace seastar {
    using ::seastar::map_reduce_scheduling_group_specific;
    using ::seastar::reduce_scheduling_group_specific;
}
// scollectd
export namespace seastar::scollectd {
    using ::seastar::scollectd::data_type;
    using ::seastar::scollectd::known_type;
    using ::seastar::scollectd::type_instance_id;
}
// scollectd_api
export namespace seastar::scollectd {
    using ::seastar::scollectd::get_collectd_value;
    using ::seastar::scollectd::get_collectd_ids;
    using ::seastar::scollectd::get_collectd_description_str;
    using ::seastar::scollectd::is_enabled;
    using ::seastar::scollectd::enable;
    using ::seastar::scollectd::get_value_map;
}
// seastar
export namespace seastar {
    using ::seastar::listen;
    using ::seastar::connect;
    using ::seastar::make_socket;
    using ::seastar::make_udp_channel;
    using ::seastar::open_file_dma;
    using ::seastar::check_direct_io_support;
    using ::seastar::open_directory;
    using ::seastar::make_directory;
    using ::seastar::touch_directory;
    using ::seastar::recursive_touch_directory;
    using ::seastar::sync_directory;
    using ::seastar::remove_file;
    using ::seastar::rename_file;
    using ::seastar::follow_symlink;
    using ::seastar::file_stat;
    using ::seastar::file_size;
    using ::seastar::file_accessible;
    using ::seastar::file_exists;
    using ::seastar::file_type;
    using ::seastar::link_file;
    using ::seastar::chmod;
    using ::seastar::file_system_at;
    using ::seastar::fs_avail;
    using ::seastar::fs_free;
}
// semaphore
export namespace seastar {
    using ::seastar::broken_semaphore;
    using ::seastar::semaphore_timed_out;
    using ::seastar::semaphore_aborted;
    using ::seastar::semaphore_default_exception_factory;
    using ::seastar::named_semaphore_timed_out;
    using ::seastar::broken_named_semaphore;
    using ::seastar::named_semaphore_aborted;
    using ::seastar::named_semaphore_exception_factory;
    using ::seastar::basic_semaphore;
    using ::seastar::semaphore_units;
    using ::seastar::get_units;
    using ::seastar::try_get_units;
    using ::seastar::consume_units;
    using ::seastar::with_semaphore;
    using ::seastar::semaphore;
    using ::seastar::named_semaphore;
}
// sharded
export namespace seastar {
    using ::seastar::async_sharded_service;
    using ::seastar::peering_sharded_service;
    using ::seastar::no_sharded_instance_exception;
    using ::seastar::sharded;
    using ::seastar::sharded_parameter;
    using ::seastar::foreign_ptr;
    using ::seastar::make_foreign;
}
// shared_future
export namespace seastar {
    using ::seastar::shared_future;
    using ::seastar::shared_promise;
}
// shared_mutex
export namespace seastar {
    using ::seastar::shared_mutex;
    using ::seastar::with_shared;
    using ::seastar::with_lock;
}
// shared_ptr
export namespace seastar {
    using ::seastar::make_lw_shared;
    using ::seastar::make_shared;
    using ::seastar::static_pointer_cast;
    using ::seastar::dynamic_pointer_cast;
    using ::seastar::const_pointer_cast;
    using ::seastar::enable_lw_shared_from_this;
    using ::seastar::lw_shared_ptr;
    using ::seastar::enable_shared_from_this;
    using ::seastar::shared_ptr;
}
export namespace fmt {
    using ::fmt::ptr;
}
// simple-stream
export namespace seastar {
    using ::seastar::measuring_output_stream;
    using ::seastar::memory_input_stream;
    using ::seastar::memory_output_stream;
    using ::seastar::simple_memory_input_stream;
    using ::seastar::fragmented_memory_input_stream;
}
// slab
export namespace seastar {
    using ::seastar::slab_page_desc;
    using ::seastar::slab_item_base;
    using ::seastar::slab_class;
    using ::seastar::slab_allocator;
}
// sleep
export namespace seastar {
    using ::seastar::sleep;
    using ::seastar::sleep_aborted;
    using ::seastar::sleep_abortable;
    using ::seastar::create_smp_service_group;
    using ::seastar::destroy_smp_service_group;
    using ::seastar::default_smp_service_group;
    using ::seastar::smp_submit_to_options;
    using ::seastar::smp;
}
// smp
export namespace seastar {
    using ::seastar::this_shard_id;
    using ::seastar::smp_service_group_config;
    using ::seastar::smp_service_group;
    using ::seastar::create_smp_service_group;
    using ::seastar::destroy_smp_service_group;
    using ::seastar::default_smp_service_group;
    using ::seastar::smp_submit_to_options;
    using ::seastar::smp;
}
// smp_options
export namespace seastar {
    using ::seastar::memory_allocator;
    using ::seastar::smp_options;
}
// sstring
export namespace seastar::scollectd {
    using ::seastar::basic_sstring;
    using ::seastar::sstring;
    using ::seastar::uninitialized_string;
    using ::seastar::to_sstring;
}
// stream
export namespace seastar {
    using ::seastar::stream;
}
// task
export namespace seastar {
    using ::seastar::task;
    using ::seastar::schedule;
    using ::seastar::schedule_urgent;
}
// temporary_buffer
export namespace seastar {
    using ::seastar::temporary_buffer;
}
// thread
export namespace seastar {
    using ::seastar::thread_attributes;
    using ::seastar::thread;
    using ::seastar::async;
}
// timed_out_error
export namespace seastar {
    using ::seastar::timed_out_error;
    using ::seastar::default_timeout_exception_factory;
}
// timed_out_error
export namespace seastar {
    using ::seastar::timed_out_error;
    using ::seastar::default_timeout_exception_factory;
}
// timed_out_error
export namespace seastar {
    using ::seastar::timed_out_error;
    using ::seastar::default_timeout_exception_factory;
}
// timed_out_error
export namespace seastar {
    using ::seastar::timed_out_error;
    using ::seastar::default_timeout_exception_factory;
}
// timer-set
export namespace seastar {
    using ::seastar::timer_set;
}
// timer-set
export namespace seastar {
    using ::seastar::timer_set;
}
// unaligned
export namespace seastar {
    using ::seastar::unaligned;
    using ::seastar::unaligned_cast;
}
// units
export namespace seastar {
    using ::seastar::operator"" _KiB;
    using ::seastar::operator"" _MiB;
    using ::seastar::operator"" _GiB;
    using ::seastar::operator"" _TiB;
}
// vector-data-sink
export namespace seastar {
    using ::seastar::vector_data_sink;
}
// weak_ptr
export namespace seastar {
    using ::seastar::weak_ptr;
    using ::seastar::weakly_referencable;
}
// weak_ptr
export namespace seastar {
    using ::seastar::unaligned;
    using ::seastar::unaligned_cast;
}
// when_all
export namespace seastar {
    using ::seastar::when_all;
    using ::seastar::when_all_succeed;
}
// when_any
export namespace seastar {
    using ::seastar::when_any_result;
    using ::seastar::when_any;
}
// with_scheduling_group
export namespace seastar {
    using ::seastar::with_scheduling_group;
}
// with_timeout
export namespace seastar {
    using ::seastar::with_timeout;
}
