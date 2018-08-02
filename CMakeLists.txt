#
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License").  See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership.  You may not use this file except in compliance with the License.
#
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

#
# Copyright (C) 2018 Scylladb, Ltd.
#

cmake_minimum_required (VERSION 3.5)

list (APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include (Cooking OPTIONAL)

project (Seastar
  VERSION 2.0.0
  LANGUAGES CXX)

option (Seastar_ALLOC_FAILURE_INJECTION
  "Enable failure injection into the Seastar allocator."
  OFF)

set (Seastar_ALLOC_PAGE_SIZE
  ""
  CACHE
  STRING
  "Override the Seastar allocator page size, in bytes.")

option (Seastar_APPS
  "Enable application targets."
  ON)

set (Seastar_CXX_FLAGS
  ""
  CACHE
  STRING
  "Semicolon-separated list of extra compilation flags for Seastar itself.")

option (Seastar_DEMOS
  "Enable demonstration targets."
  ON)

option (Seastar_DOCS
  "Enable documentation targets."
  ON)

option (Seastar_DPDK
  "Enable DPDK support."
  OFF)

option (Seastar_EXCEPTION_SCALABILITY_WORKAROUND
  "Enable a workaround for C++ exception scalability issues by preventing override of the `dl_iterate_phdr` symbol."
  OFF)

option (Seastar_EXCLUDE_TESTS_FROM_ALL
  "When enabled alongide Seastar_TESTING, do not build tests by default."
  OFF)

option (Seastar_GCC6_CONCEPTS
  "Enable compilation with gcc version 6 concepts support."
  OFF)

option (Seastar_HWLOC
  "Enable hwloc support."
  ON)

option (Seastar_INSTALL
  "Install targets."
  ON)

option (Seastar_NUMA
  "Enable NUMA support, with `numactl`."
  ON)

option (Seastar_TESTING
  "Enable testing targets."
  ON)

if (NOT (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR))
  set (Seastar_APPS OFF)
  set (Seastar_DEMOS OFF)
  set (Seastar_DOCS OFF)
  set (Seastar_INSTALL OFF)
  set (Seastar_TESTING OFF)
endif ()

#
# Useful (non-cache) variables.
#

set (Seastar_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set (Seastar_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

#
# Dependencies.
#

##
## Public dependencies.
##

find_package (Boost 1.64.0 REQUIRED
  COMPONENTS
    filesystem
    program_options
    thread)

find_package (c-ares 1.13.0 REQUIRED MODULE)
find_package (cryptopp 5.6.5 REQUIRED)
# No version information published.
find_package (dpdk)
find_package (fmt 4.0.0 REQUIRED)
find_package (lz4 1.8.0 REQUIRED)

##
## Private and private/public dependencies.
##

find_package (GnuTLS 3.5.18 REQUIRED)
find_package (LinuxMembarrier)
find_package (Protobuf 3.3.1 REQUIRED)
find_package (Sanitizers REQUIRED)
find_package (StdFilesystem REQUIRED)
find_package (dl REQUIRED)
find_package (hwloc 1.11.5)
# No version information published.
find_package (lksctp-tools REQUIRED)
# No version information published.
find_package (numactl)
find_package (ragel 7.0.0 REQUIRED)
find_package (rt REQUIRED)
find_package (yaml-cpp 0.5.3 REQUIRED)

#
# Code generation helpers.
#

function (seastar_generate_protobuf target var in_file out_dir)
  get_filename_component (in_file_name ${in_file} NAME_WE)
  get_filename_component (in_file_dir ${in_file} DIRECTORY)
  set (header_out ${out_dir}/${in_file_name}.pb.h)
  set (source_out ${out_dir}/${in_file_name}.pb.cc)

  add_custom_command (
    DEPENDS ${in_file}
    OUTPUT ${header_out} ${source_out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE} --cpp_out=${out_dir} -I${in_file_dir} ${in_file})

  add_custom_target (${target}
    DEPENDS
      ${header_out}
      ${source_out})

  set (${var} ${header_out} ${source_out} PARENT_SCOPE)
endfunction ()

function (seastar_generate_ragel target var in_file out_file)
  get_filename_component (out_dir ${out_file} DIRECTORY)

  add_custom_command (
    DEPENDS ${in_file}
    OUTPUT ${out_file}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${ragel_RAGEL_EXECUTABLE} -G2 -o ${out_file} ${in_file}
    COMMAND sed -i -e "'1h;2,$$H;$$!d;g'" -re "'s/static const char _nfa[^;]*;//g'" ${out_file})

  add_custom_target (${target}
    DEPENDS ${out_file})

  set (${var} ${out_file} PARENT_SCOPE)
endfunction ()

function (seastar_generate_swagger target var in_file out_file)
  get_filename_component (out_dir ${out_file} DIRECTORY)
  set (generator ${Seastar_SOURCE_DIR}/gen/json2code.py)

  add_custom_command (
    DEPENDS
      ${in_file}
      ${generator}
    OUTPUT ${out_file}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${generator} -f ${in_file} -o ${out_file})

  add_custom_target (${target}
    DEPENDS ${out_file})

  set (${var} ${out_file} PARENT_SCOPE)
endfunction ()

#
# The `seastar` library.
#

seastar_generate_ragel (seastar_http_request_parser
  http_request_parser_file
  ${CMAKE_CURRENT_SOURCE_DIR}/gen/http/request_parser.rl
  ${CMAKE_CURRENT_BINARY_DIR}/gen/seastar/http/request_parser.hh)

seastar_generate_ragel (seastar_http_response_parser
  http_response_parser_file
  ${CMAKE_CURRENT_SOURCE_DIR}/gen/http/response_parser.rl
  ${CMAKE_CURRENT_BINARY_DIR}/gen/seastar/http/response_parser.hh)

seastar_generate_protobuf (seastar_proto_metrics2
  proto_metrics2_files
  ${CMAKE_CURRENT_SOURCE_DIR}/gen/proto/metrics2.proto
  ${CMAKE_CURRENT_BINARY_DIR}/gen/seastar/proto)

add_library (seastar STATIC
  ${http_request_parser_file}
  ${proto_metrics2_files}
  include/seastar/core/abort_source.hh
  include/seastar/core/alien.hh
  include/seastar/core/align.hh
  include/seastar/core/aligned_buffer.hh
  include/seastar/core/app-template.hh
  include/seastar/core/apply.hh
  include/seastar/core/array_map.hh
  include/seastar/core/bitops.hh
  include/seastar/core/bitset-iter.hh
  include/seastar/core/byteorder.hh
  include/seastar/core/cacheline.hh
  include/seastar/core/checked_ptr.hh
  include/seastar/core/chunked_fifo.hh
  include/seastar/core/circular_buffer.hh
  include/seastar/core/circular_buffer_fixed_capacity.hh
  include/seastar/core/condition-variable.hh
  include/seastar/core/deleter.hh
  include/seastar/core/distributed.hh
  include/seastar/core/do_with.hh
  include/seastar/core/dpdk_rte.hh
  include/seastar/core/enum.hh
  include/seastar/core/exception_hacks.hh
  include/seastar/core/execution_stage.hh
  include/seastar/core/expiring_fifo.hh
  include/seastar/core/fair_queue.hh
  include/seastar/core/file.hh
  include/seastar/core/fsqual.hh
  include/seastar/core/fstream.hh
  include/seastar/core/function_traits.hh
  include/seastar/core/future-util.hh
  include/seastar/core/future.hh
  include/seastar/core/gate.hh
  include/seastar/core/iostream-impl.hh
  include/seastar/core/iostream.hh
  include/seastar/core/linux-aio.hh
  include/seastar/core/lowres_clock.hh
  include/seastar/core/manual_clock.hh
  include/seastar/core/memory.hh
  include/seastar/core/metrics.hh
  include/seastar/core/metrics_api.hh
  include/seastar/core/metrics_registration.hh
  include/seastar/core/metrics_types.hh
  include/seastar/core/pipe.hh
  include/seastar/core/posix.hh
  include/seastar/core/preempt.hh
  include/seastar/core/prefetch.hh
  include/seastar/core/print.hh
  include/seastar/core/prometheus.hh
  include/seastar/core/queue.hh
  include/seastar/core/ragel.hh
  include/seastar/core/reactor.hh
  include/seastar/core/report_exception.hh
  include/seastar/core/resource.hh
  include/seastar/core/rwlock.hh
  include/seastar/core/scattered_message.hh
  include/seastar/core/scheduling.hh
  include/seastar/core/scollectd.hh
  include/seastar/core/scollectd_api.hh
  include/seastar/core/seastar.hh
  include/seastar/core/semaphore.hh
  include/seastar/core/sharded.hh
  include/seastar/core/shared_future.hh
  include/seastar/core/shared_mutex.hh
  include/seastar/core/shared_ptr.hh
  include/seastar/core/shared_ptr_debug_helper.hh
  include/seastar/core/shared_ptr_incomplete.hh
  include/seastar/core/simple-stream.hh
  include/seastar/core/slab.hh
  include/seastar/core/sleep.hh
  include/seastar/core/sstring.hh
  include/seastar/core/stall_sampler.hh
  include/seastar/core/stream.hh
  include/seastar/core/systemwide_memory_barrier.hh
  include/seastar/core/task.hh
  include/seastar/core/temporary_buffer.hh
  include/seastar/core/thread.hh
  include/seastar/core/thread_cputime_clock.hh
  include/seastar/core/thread_impl.hh
  include/seastar/core/timer-set.hh
  include/seastar/core/timer.hh
  include/seastar/core/transfer.hh
  include/seastar/core/unaligned.hh
  include/seastar/core/units.hh
  include/seastar/core/vector-data-sink.hh
  include/seastar/core/vla.hh
  include/seastar/core/weak_ptr.hh
  include/seastar/http/api_docs.hh
  include/seastar/http/common.hh
  include/seastar/http/exception.hh
  include/seastar/http/file_handler.hh
  include/seastar/http/function_handlers.hh
  include/seastar/http/handlers.hh
  include/seastar/http/httpd.hh
  include/seastar/http/json_path.hh
  include/seastar/http/matcher.hh
  include/seastar/http/matchrules.hh
  include/seastar/http/mime_types.hh
  include/seastar/http/reply.hh
  include/seastar/http/request.hh
  include/seastar/http/routes.hh
  include/seastar/http/transformers.hh
  include/seastar/json/formatter.hh
  include/seastar/json/json_elements.hh
  include/seastar/net/api.hh
  include/seastar/net/arp.hh
  include/seastar/net/byteorder.hh
  include/seastar/net/config.hh
  include/seastar/net/const.hh
  include/seastar/net/dhcp.hh
  include/seastar/net/dns.hh
  include/seastar/net/dpdk.hh
  include/seastar/net/ethernet.hh
  include/seastar/net/inet_address.hh
  include/seastar/net/ip.hh
  include/seastar/net/ip_checksum.hh
  include/seastar/net/native-stack.hh
  include/seastar/net/net.hh
  include/seastar/net/packet-data-source.hh
  include/seastar/net/packet-util.hh
  include/seastar/net/packet.hh
  include/seastar/net/posix-stack.hh
  include/seastar/net/proxy.hh
  include/seastar/net/socket_defs.hh
  include/seastar/net/stack.hh
  include/seastar/net/tcp-stack.hh
  include/seastar/net/tcp.hh
  include/seastar/net/tls.hh
  include/seastar/net/toeplitz.hh
  include/seastar/net/udp.hh
  include/seastar/net/virtio-interface.hh
  include/seastar/net/virtio.hh
  include/seastar/rpc/lz4_compressor.hh
  include/seastar/rpc/multi_algo_compressor_factory.hh
  include/seastar/rpc/rpc.hh
  include/seastar/rpc/rpc_impl.hh
  include/seastar/rpc/rpc_types.hh
  include/seastar/util/alloc_failure_injector.hh
  include/seastar/util/backtrace.hh
  include/seastar/util/bool_class.hh
  include/seastar/util/conversions.hh
  include/seastar/util/defer.hh
  include/seastar/util/eclipse.hh
  include/seastar/util/function_input_iterator.hh
  include/seastar/util/gcc6-concepts.hh
  include/seastar/util/indirect.hh
  include/seastar/util/is_smart_ptr.hh
  include/seastar/util/lazy.hh
  include/seastar/util/log-cli.hh
  include/seastar/util/log.hh
  include/seastar/util/noncopyable_function.hh
  include/seastar/util/optimized_optional.hh
  include/seastar/util/print_safe.hh
  include/seastar/util/program-options.hh
  include/seastar/util/reference_wrapper.hh
  include/seastar/util/spinlock.hh
  include/seastar/util/transform_iterator.hh
  include/seastar/util/tuple_utils.hh
  include/seastar/util/variant_utils.hh
  src/core/alien.cc
  src/core/app-template.cc
  src/core/dpdk_rte.cc
  src/core/exception_hacks.cc
  src/core/execution_stage.cc
  src/core/file-impl.hh
  src/core/fsqual.cc
  src/core/fstream.cc
  src/core/future-util.cc
  src/core/linux-aio.cc
  src/core/memory.cc
  src/core/metrics.cc
  src/core/posix.cc
  src/core/prometheus.cc
  src/core/reactor.cc
  src/core/resource.cc
  src/core/scollectd.cc
  src/core/scollectd-impl.hh
  src/core/systemwide_memory_barrier.cc
  src/core/thread.cc
  src/http/api_docs.cc
  src/http/common.cc
  src/http/file_handler.cc
  src/http/httpd.cc
  src/http/json_path.cc
  src/http/matcher.cc
  src/http/mime_types.cc
  src/http/reply.cc
  src/http/routes.cc
  src/http/transformers.cc
  src/json/formatter.cc
  src/json/json_elements.cc
  src/net/arp.cc
  src/net/config.cc
  src/net/dhcp.cc
  src/net/dns.cc
  src/net/dpdk.cc
  src/net/ethernet.cc
  src/net/inet_address.cc
  src/net/ip.cc
  src/net/ip_checksum.cc
  src/net/native-stack-impl.hh
  src/net/native-stack.cc
  src/net/net.cc
  src/net/packet.cc
  src/net/posix-stack.cc
  src/net/proxy.cc
  src/net/stack.cc
  src/net/tcp.cc
  src/net/tls.cc
  src/net/udp.cc
  src/net/virtio.cc
  src/rpc/lz4_compressor.cc
  src/rpc/rpc.cc
  src/util/alloc_failure_injector.cc
  src/util/backtrace.cc
  src/util/conversions.cc
  src/util/log.cc
  src/util/program-options.cc)

add_library (Seastar::seastar ALIAS seastar)

add_dependencies (seastar
  seastar_http_request_parser
  seastar_http_response_parser
  seastar_proto_metrics2)

if (cxx_std_17 IN_LIST CMAKE_CXX_KNOWN_FEATURES)
  target_compile_features (seastar
    PUBLIC cxx_std_17)
else ()
  target_compile_options (seastar
    PUBLIC -std=c++17)
endif ()

target_include_directories (seastar
  PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/gen>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries (seastar
  PUBLIC
    Boost::boost
    Boost::program_options
    Boost::thread
    c-ares::c-ares
    cryptopp::cryptopp
    fmt::fmt
    lz4::lz4
  PRIVATE
    Boost::filesystem
    GnuTLS::gnutls
    StdFilesystem::filesystem
    dl::dl
    lksctp-tools::lksctp-tools
    protobuf::libprotobuf
    rt::rt
    yaml-cpp::yaml-cpp
    debug Sanitizers::address
    debug Sanitizers::undefined_behavior)

if (LinuxMembarrier_FOUND)
  target_compile_definitions (seastar
    PRIVATE SEASTAR_HAS_MEMBARRIER)

  target_link_libraries (seastar
    PRIVATE LinuxMembarrier::membarrier)
endif ()

if (Seastar_ALLOC_FAILURE_INJECTION)
  target_compile_definitions (seastar
    PUBLIC SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION)
endif ()

if (Sanitizers_FIBER_SUPPORT)
  target_compile_definitions (seastar
    PRIVATE SEASTAR_HAVE_ASAN_FIBER_SUPPORT)
endif ()

if (Seastar_ALLOC_PAGE_SIZE)
  target_compile_definitions (seastar
    PUBLIC SEASTAR_OVERRIDE_ALLOCATOR_PAGE_SIZE=${Seastar_ALLOC_PAGE_SIZE})
endif ()

if (Seastar_CXX_FLAGS)
  target_compile_options (seastar
    PRIVATE ${Seastar_CXX_FLAGS})
endif ()

if (Seastar_DPDK)
  if (NOT dpdk_FOUND)
    message (FATAL_ERROR "dpdk support is enabled but it is not available!")
  endif ()

  target_compile_definitions (seastar
    PUBLIC SEASTAR_HAVE_DPDK)

  target_link_libraries (seastar
    PUBLIC dpdk::dpdk)
endif ()

if (Seastar_GCC6_CONCEPTS)
  target_compile_definitions (seastar
    PUBLIC SEASTAR_HAVE_GCC6_CONCEPTS)

  target_compile_options (seastar
    PUBLIC -fconcepts)
endif ()

if (NOT Seastar_EXCEPTION_SCALABILITY_WORKAROUND)
  target_compile_definitions (seastar
    PRIVATE SEASTAR_NO_EXCEPTION_HACK)
endif ()

if (Seastar_HWLOC)
  if (NOT hwloc_FOUND)
    message (FATAL_ERROR "`hwloc` support is enabled but it is not available!")
  endif ()

  target_compile_definitions (seastar
    PRIVATE SEASTAR_HAVE_HWLOC)

  target_link_libraries (seastar
    PRIVATE hwloc::hwloc)
endif ()

if (Seastar_NUMA)
  if (NOT numactl_FOUND)
    message (FATAL_ERROR "NUMA support is enabled but `numactl` is not available!")
  endif ()

  target_compile_definitions (seastar
    PRIVATE SEASTAR_HAVE_NUMA)

  target_link_libraries (seastar
    PRIVATE numactl::numactl)
endif ()

if (lz4_HAVE_COMPRESS_DEFAULT)
  target_compile_definitions (seastar
    PRIVATE SEASTAR_HAVE_LZ4_COMPRESS_DEFAULT)
endif ()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions (seastar
    PUBLIC
      SEASTAR_DEBUG
      SEASTAR_DEBUG_SHARED_PTR
      SEASTAR_ASAN_ENABLED
      SEASTAR_THREAD_STACK_GUARDS
    PRIVATE SEASTAR_DEFAULT_ALLOCATOR)
endif ()

#
# Tests and the testing library.
#

if (Seastar_TESTING)
  find_package (Boost REQUIRED
    COMPONENTS unit_test_framework)

  enable_testing ()

  if (Seastar_EXCLUDE_TESTS_FROM_ALL)
    set (exclude EXCLUDE_FROM_ALL)
  else ()
    set (exclude "")
  endif ()

  add_subdirectory (test ${exclude})
endif ()

#
# Demonstrations.
#

if (Seastar_DEMOS)
  add_subdirectory (demo)
endif ()

#
# Documentation.
#

if (Seastar_DOCS)
  add_subdirectory (doc)
endif ()

#
# Applications.
#

if (Seastar_APPS)
  add_subdirectory (app)
endif ()

#
# pkg-config generation.
#
# Note that unlike the CMake "config module", this description is not relocatable because
# some dependencies do not natively support pkg-config.
#

# Necessary here for pkg-config.
include (GNUInstallDirs)

configure_file (
  ${CMAKE_CURRENT_SOURCE_DIR}/pkgconfig/seastar.pc.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/seastar.pc.cmake
  @ONLY)

file (GENERATE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/seastar.pc
  INPUT ${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/seastar.pc.cmake)

#
# Installation and export.
#

if (Seastar_INSTALL)
  include (CMakePackageConfigHelpers)
  set (install_cmakedir ${CMAKE_INSTALL_LIBDIR}/cmake/Seastar)

  install (
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install (
    TARGETS seastar
    EXPORT seastar-export
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

  install (
    EXPORT seastar-export
    FILE SeastarTargets.cmake
    NAMESPACE Seastar::
    DESTINATION ${install_cmakedir})

  write_basic_package_version_file (
    ${CMAKE_CURRENT_BINARY_DIR}/SeastarConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ExactVersion)

  configure_package_config_file (
    ${CMAKE_CURRENT_LIST_DIR}/cmake/SeastarConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/SeastarConfig.cmake
    INSTALL_DESTINATION ${install_cmakedir})

  install (
    FILES
      ${CMAKE_CURRENT_BINARY_DIR}/SeastarConfig.cmake
      ${CMAKE_CURRENT_BINARY_DIR}/SeastarConfigVersion.cmake
    DESTINATION ${install_cmakedir})

  install (
    FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindBoost.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindGnuTLS.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindLinuxMembarrier.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindProtobuf.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindSanitizers.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindStdFilesystem.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findc-ares.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findcryptopp.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Finddpdk.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findhwloc.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findlksctp-tools.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findlz4.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findnumactl.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findragel.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findrt.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Finddl.cmake
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findyaml-cpp.cmake
    DESTINATION ${install_cmakedir})

  install (
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/cmake/code_test
    DESTINATION ${install_cmakedir})

  install (
    FILES ${CMAKE_CURRENT_BINARY_DIR}/seastar.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

  #
  # Export targets from the build tree for the user package registry.
  #

  export (
    EXPORT seastar-export
    FILE ${CMAKE_CURRENT_BINARY_DIR}/SeastarTargets.cmake
    NAMESPACE Seastar::)

  export (PACKAGE Seastar)

  #
  # Packaging.
  #

  set (CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
  set (CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
  set (CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

  include (CPack)
endif ()
