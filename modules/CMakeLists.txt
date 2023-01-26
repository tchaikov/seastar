add_library (seastar_module)
target_sources (seastar_module
  PUBLIC
    FILE_SET CXX_MODULES
      FILES "seastar.cc")
target_link_libraries (seastar_module
  PUBLIC seastar_common)
target_compile_features (seastar_module
  PUBLIC cxx_std_20)
install (
  TARGETS seastar_module
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  CXX_MODULES_BMI
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cxx/bmi"
    COMPONENT "bmi"
  FILE_SET CXX_MODULES
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cxx/modules"
    COMPONENT "modules")
export (
  TARGETS
    seastar_module
  NAMESPACE Seastar::
  APPEND FILE "${CMAKE_CURRENT_BINARY_DIR}/SeastarModules.cmake"
  CXX_MODULES_DIRECTORY "cxx-modules")

macro (prepare_partition_sources partition base_dir)
  if (IS_ABSOLUTE "${base_dir}")
    set (cxx_dir "${base_dir}")
    string (LENGTH "${Seastar_BINARY_DIR}" binary_dir_length)
    if (NOT "${base_dir}" MATCHES "^${Seastar_BINARY_DIR}")
      message (FATAL "BASE_DIR ${base_dir} is not under ${Seastar_BINARY_DIR}")
    endif ()
    string (SUBSTRING "${base_dir}" ${binary_dir_length} -1 sub_dir)
    string (CONCAT module_dir "${CMAKE_CURRENT_BINARY_DIR}" "${sub_dir}")
  else ()
    set (cxx_dir "${CMAKE_SOURCE_DIR}/${base_dir}")
    set (module_dir "${CMAKE_CURRENT_BINARY_DIR}/${base_dir}")
  endif ()
  file (MAKE_DIRECTORY "${module_dir}")
  foreach (file_name ${ARGN})
    set (cxx_src "${cxx_dir}/${file_name}")
    set (module_src "${module_dir}/${file_name}.cc")
    # CMake refuses to compile .hh files as modules, so we need to rename them
    # first. and more importantly, we cannot put "module;" at the top of module
    # source file using macro, as C++ module *containerize* macros, and "module"
    # cannot be declared using a macro.
    add_custom_command (
      OUTPUT ${module_src}
      DEPENDS ${cxx_src}
      # silence "-Wpragma-once-outside-header"
      COMMAND sed
            -e "1i module;"
            -e "s/^#pragma once//"
            -e "s/^SEASTAR_EXPORT_MODULE(\\(.*\\))/export module \\1/"
            -e "s/^SEASTAR_EXPORT/export/"
            -e "s/^SEASTAR_MODULE(\\(.*\\))/module \\1/"
            "${cxx_src}" > "${module_src}"
      VERBATIM)
    list (APPEND module_srcs "${module_src}")
  endforeach ()
  target_include_directories (seastar_module
    PRIVATE "${cxx_dir}")
endmacro ()

function (seastar_partition_interfaces partition)
  cmake_parse_arguments (parsed_args
    ""
    "BASE_DIR"
    "FILES"
    ${ARGN})
  prepare_partition_sources (${partition}
    "${parsed_args_BASE_DIR}" ${parsed_args_FILES})
  target_sources (seastar_module
    PUBLIC
      FILE_SET "module_${partition}"
      TYPE CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
      FILES "${module_srcs}")
endfunction ()

function (seastar_partition_implementation partition)
  cmake_parse_arguments (parsed_args
    ""
    "BASE_DIR"
    "FILES"
    ${ARGN})
  prepare_partition_sources(${partition}
    "${parsed_args_BASE_DIR}" ${parsed_args_FILES})
  target_sources (seastar_module
    PRIVATE
      ${module_srcs})
endfunction ()

seastar_partition_interfaces (core
  BASE_DIR include/seastar/core
  FILES
    alien.hh
    align.hh
    abort_on_ebadf.hh
    abort_on_expiry.hh
    abort_source.hh
    abortable_fifo.hh
    app-template.hh
    bitops.hh
    bitset-iter.hh
    cacheline.hh
    chunked_fifo.hh
    circular_buffer.hh
    circular_buffer_fixed_capacity.hh
    condition-variable.hh
    deleter.hh
    do_with.hh
    fair_queue.hh
    file-types.hh
    file.hh
    fstream.hh
    function_traits.hh
    future.hh
    gate.hh
    idle_cpu_handler.hh
    io_priority_class.hh
    io_queue.hh
    iostream.hh
    loop.hh
    lowres_clock.hh
    manual_clock.hh
    map_reduce.hh
    memory.hh
    metrics.hh
    metrics_api.hh
    metrics_registration.hh
    metrics_types.hh
    on_internal_error.hh
    posix.hh
    preempt.hh
    prefetch.hh
    prometheus.hh
    ragel.hh
    reactor.hh
    reactor_config.hh
    report_exception.hh
    resource.hh
    rwlock.hh
    scattered_message.hh
    scheduling.hh
    scheduling_specific.hh
    scollectd.hh
    scollectd_api.hh
    seastar.hh
    semaphore.hh
    sharded.hh
    shared_future.hh
    shared_mutex.hh
    shared_ptr.hh
    shared_ptr_debug_helper.hh
    simple-stream.hh
    sleep.hh
    smp.hh
    smp_options.hh
    sstring.hh
    stream.hh
    task.hh
    temporary_buffer.hh
    thread.hh
    thread_impl.hh
    timed_out_error.hh
    timer-set.hh
    timer.hh
    units.hh
    weak_ptr.hh
    when_all.hh
    when_any.hh
    with_scheduling_group.hh
    with_timeout.hh)
seastar_partition_implementation (core
  BASE_DIR src/core
  FILES
    alien.cc
    file.cc
    fair_queue.cc
    reactor_backend.cc
    thread_pool.cc
    app-template.cc
    exception_hacks.cc
    execution_stage.cc
    fsnotify.cc
    fsqual.cc
    fstream.cc
    future.cc
    future-util.cc
    linux-aio.cc
    memory.cc
    metrics.cc
    on_internal_error.cc
    posix.cc
    prometheus.cc
    program_options.cc
    reactor.cc
    resource.cc
    sharded.cc
    scollectd.cc
    scollectd-impl.hh
    systemwide_memory_barrier.cc
    smp.cc
    sstring.cc
    thread.cc
    uname.cc
    io_queue.cc
    semaphore.cc
    condition-variable.cc)

seastar_partition_interfaces (core_internal
  BASE_DIR include/seastar/core/internal
  FILES
    io_desc.hh
    io_intent.hh
    io_request.hh
    io_sink.hh
    poll.hh
    pollable_fd.hh
    uname.hh)

seastar_partition_interfaces (net
  BASE_DIR include/seastar/net
  FILES
    api.hh
    const.hh
    socket_defs.hh
    tls.hh
    unix_address.hh
    packet.hh)
seastar_partition_implementation (net
  BASE_DIR src/util
  FILES
    log.cc)

seastar_partition_interfaces (util
  BASE_DIR include/seastar/util
  FILES
    backtrace.hh
    bool_class.hh
    critical_alloc_section.hh
    indirect.hh
    is_smart_ptr.hh
    log-impl.hh
    log.hh
    noncopyable_function.hh
    optimized_optional.hh
    program-options.hh
    program-options1.hh
    shared_token_bucket.hh
    spinlock.hh
    used_size.hh
    variant_utils.hh)
seastar_partition_implementation (util
  BASE_DIR src/util
  FILES
    log.cc
    program-options.cc)

seastar_partition_interfaces (http
  BASE_DIR include/seastar/http
  FILES
    common.hh
    httpd.hh
    mime_types.hh
    request.hh)
seastar_partition_implementation (http
  BASE_DIR src/http
  FILES
    httpd.cc)

seastar_partition_interfaces (http_parser
  BASE_DIR ${Seastar_GEN_BINARY_DIR}/include/seastar/http
  FILES
    chunk_parsers.hh
    request_parser.hh
    response_parser.hh)
