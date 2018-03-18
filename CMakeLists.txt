cmake_minimum_required (VERSION 2.8.12)

project (seastar)

get_filename_component (SEASTAR_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR} ABSOLUTE)
get_filename_component (SEASTAR_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR} ABSOLUTE)

#
# Prologue.
#
# We define these manually because we want to be explicit about the options that are defined and because CMake defines
# NDEBUG by default, and we don't wish to.

set (CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
set (CMAKE_CXX_FLAGS_RELEASE "-O2")
set (CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")
set (CMAKE_CXX_FLAGS_MINSIZEREL "-Os")

# Set an option defined in a sub-directory.
# Courtesy of http://edsiper.linuxchile.cl/blog/2016/01/08/cmake-override-subdirectory-options/
macro (set_option option value)
  set (${option} ${value} CACHE INTERNAL "" FORCE)
endmacro ()

#
# Configuration options.
#

set (SEASTAR_USER_CXXFLAGS
  ""
  CACHE
  STRING
  "Extra CXXFLAGS, separated by semicolon.")

set (SEASTAR_USER_LDFLAGS
  ""
  CACHE
  STRING
  "Extra LDFLAGS, separated by semicolon.")

option (SEASTAR_ENABLE_HWLOC
  "Enable hwloc support."
  ON)

option (SEASTAR_ENABLE_DPDK
  "Enable DPDK (from bundled sources)."
  OFF)

option (SEASTAR_ENABLE_EXCEPTION_SCALABILITY_WORKAROUND
  "Override the dl_iterate_phdr symbol to workaround C++ exception scalability issues."
  OFF)

set (SEASTAR_ALLOCATOR_PAGE_SIZE
  ""
  CACHE
  STRING
  "Override the allocator page size in bytes.")

option (SEASTAR_ENABLE_ALLOC_FAILURE_INJECTOR
  "Enable allocation failure injection."
  OFF)

option (SEASTAR_ENABLE_GCC6_CONCEPTS
  "Enable experimental support for C++ concepts as implemented in GCC 6."
  OFF)

option (SEASTAR_LINK_STATIC_BOOST
  "Link with the Boost library statically."
  OFF)

option (SEASTAR_EXECUTE_ONLY_FAST_TESTS
  "Execute fast unit tests only, where applicable."
  OFF)

set (SEASTAR_JENKINS
  ""
  CACHE
  STRING
  "Configure the build and tests for execution within a Jenkins context, with the given identifier.")

#
# Link pools (only supported for the Ninja generator).
#

if(${CMAKE_GENERATOR} STREQUAL "Ninja")
  exec_program ("${CMAKE_CURRENT_SOURCE_DIR}/cmake-tools/query_link_pool_depth.py"
    OUTPUT_VARIABLE link_pool_depth
    RETURN_VALUE query_pool_depth_code)

  if (NOT ("${query_pool_depth_code}" STREQUAL 0))
    MESSAGE (FATAL_ERROR "tools/query_link_pool_depth.py had a non-zero exit code.")
  endif ()

  set_property (GLOBAL PROPERTY
    JOB_POOLS
    seastar_link_pool=${link_pool_depth})

  set_property (GLOBAL PROPERTY
    CMAKE_JOB_POOL_LINK seastar_link_pool)
endif()

#
# External projects.
#

set_option (CARES_STATIC YES)
set_option (CARES_SHARED NO)
set_option (CARES_INSTALL NO)
set_option (CARES_STATIC_PIC YES)
add_subdirectory (c-ares)

add_subdirectory (fmt)


#
# Library.
#

set(CORE_FILES
  core/alien_message_queue.cc
  core/alien_poller.cc
  core/app-template.cc
  core/dpdk_rte.cc
  core/exception_hacks.cc
  core/linux-aio.cc
  core/fsqual.cc
  core/fstream.cc
  core/memory.cc
  core/metrics.cc
  core/posix.cc
  core/prometheus.cc
  core/reactor.cc
  core/resource.cc
  core/scollectd.cc
  core/systemwide_memory_barrier.cc
  core/thread.cc)

set(HTTP_FILES
  http/api_docs.cc
  http/common.cc
  http/file_handler.cc
  http/httpd.cc
  http/json_path.cc
  http/matcher.cc
  http/mime_types.cc
  http/reply.cc
  http/routes.cc
  http/transformers.cc)

set(JSON_FILES
  json/formatter.cc
  json/json_elements.cc)

set(NET_FILES
  net/arp.cc
  net/config.cc
  net/dhcp.cc
  net/dns.cc
  net/dpdk.cc
  net/ethernet.cc
  net/inet_address.cc
  net/ip.cc
  net/ip_checksum.cc
  net/native-stack.cc
  net/net.cc
  net/packet.cc
  net/posix-stack.cc
  net/proxy.cc
  net/stack.cc
  net/tcp.cc
  net/tls.cc
  net/udp.cc
  net/virtio.cc)

set(RPC_FILES
  rpc/lz4_compressor.cc
  rpc/rpc.cc)

set(UTIL_FILES
  util/alloc_failure_injector.cc
  util/backtrace.cc
  util/conversions.cc
  util/log.cc
  util/program-options.cc)

add_library (seastar
  ${CORE_FILES}
  ${HTTP_FILES}
  ${JSON_FILES}
  ${NET_FILES}
  ${RPC_FILES}
  ${UTIL_FILES}
  $<TARGET_OBJECTS:metrics_proto>)

add_library (Seastar::seastar ALIAS seastar)

##
## DPDK.
##

if (${SEASTAR_ENABLE_DPDK})
  set (dpdk_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/dpdk")
  set (dpdk_binary_dir "${CMAKE_CURRENT_BINARY_DIR}/dpdk")

  #
  # Oh, CMake...
  #

  set (arch "")
  foreach (flag ${SEASTAR_USER_CXXFLAGS})
    if (${flag} MATCHES "-march=(.+)")
      set (arch ${CMAKE_MATCH_1})
    endif ()
  endforeach ()

  if ("${arch}" STREQUAL "nehalem")
    set (dpdk_machine "nhm")
  elseif ("${arch}" STREQUAL "westmere")
    set (dpdk_machine "wsm")
  elseif ("${arch}" STREQUAL "sandybridge")
    set (dpdk_machine "snb")
  elseif ("${arch}" STREQUAL "ivybridge")
    set (dpdk_machine "ivb")
  else ()
    set (dpdk_machine "native")
  endif ()

  if (EXISTS "${dpdk_binary_dir}/.config")
    file (REMOVE "${dpdk_binary_dir}/.config")
  endif ()

  exec_program (make
    ARGS -C ${dpdk_source_dir} RTE_OUTPUT=${dpdk_binary_dir} config "T=x86_64-${dpdk_machine}-linuxapp-gcc")

  exec_program ("${CMAKE_CURRENT_SOURCE_DIR}/cmake-tools/dpdk_adjust_variables.py"
    ARGS "${dpdk_binary_dir}/.config" ${dpdk_machine})

  exec_program ("${CMAKE_CURRENT_SOURCE_DIR}/cmake-tools/dpdk_query_cflags.py"
    ARGS ${dpdk_binary_dir} ${dpdk_machine}
    OUTPUT_VARIABLE dpdk_cflags)

  add_custom_target (seastar-dpdk
    COMMAND make -C ${dpdk_binary_dir} CC=\"${CMAKE_C_COMPILER} -Wno-implicit-fallthrough -Wno-format-truncation -Wno-bool-operation -Wno-maybe-uninitialized\")

  add_dependencies (seastar seastar-dpdk)

  target_include_directories (seastar SYSTEM PUBLIC "${dpdk_binary_dir}/include")
  target_compile_options (seastar PUBLIC "${dpdk_cflags}")
endif ()

#
# System dependencies.
#

set (CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

##
## Unconditional dependencies.
##

if (${SEASTAR_LINK_STATIC_BOOST})
  set (Boost_USE_STATIC_LIBS ON)
endif ()

if (NOT TARGET Boost)
  find_package (Boost COMPONENTS program_options filesystem unit_test_framework thread REQUIRED)
endif ()

if (${Boost_VERSION} VERSION_LESS "1.55")
  message(error "Seastar requires Boost >= 1.55")
endif ()

find_package (GnuTLS REQUIRED)

find_package (Protobuf REQUIRED)

find_package (Cryptopp REQUIRED)

find_package (Lz4 REQUIRED)

find_package (Yaml-cpp REQUIRED)

find_library (RT_LIBRARY rt DOC "The Posix Realtime extension library.")

find_library (AIO_LIBRARY aio DOC "The Posix asynchronous I/O library.")

find_program (Ragel_EXECUTABLE NAMES ragel DOC "Path to the ragel executable.")

##
## Conditional dependencies.
##

if (${SEASTAR_ENABLE_HWLOC})
  find_package (HWLoc REQUIRED)

  find_package (LibXml2 REQUIRED)

  find_package (ZLIB REQUIRED)

  find_library (NUMA_LIBRARY numa DOC "The NUMA support library.")

  find_library (PCIAccess_LIBRARY pciaccess DOC "The pciaccess library.")
endif ()

#
# Compilation flags and definitions.
#

target_compile_options (seastar PUBLIC
  -std=gnu++1y
  -Wall
  -Werror
  -Wno-error=deprecated-declarations
  -fvisibility=hidden
  -U_FORTIFY_SOURCE)

if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
  target_compile_options (seastar PUBLIC
    -fsanitize=address
    -fsanitize=leak
    -fsanitize=undefined)

  target_compile_definitions (seastar PUBLIC
    DEBUG
    DEBUG_SHARED_PTR
    DEFAULT_ALLOCATOR
    SEASTAR_THREAD_STACK_GUARDS
    ASAN_ENABLED)
endif ()

if (${SEASTAR_ENABLE_GCC6_CONCEPTS})
  target_compile_options (seastar PUBLIC -fconcepts)
  target_compile_definitions (seastar PUBLIC HAVE_GCC6_CONCEPTS)
endif ()

if (NOT ${SEASTAR_ENABLE_EXCEPTION_SCALABILITY_WORKAROUND})
  target_compile_definitions (seastar PUBLIC
    NO_EXCEPTION_HACK)
endif ()

if (${SEASTAR_ENABLE_HWLOC})
  target_compile_definitions (seastar PUBLIC
    HAVE_HWLOC HAVE_NUMA)
endif ()

if (${SEASTAR_ENABLE_DPDK})
  target_compile_options (seastar PUBLIC
    -Wno-error=literal-suffix
    -Wno-literal-suffix
    -Wno-invalid-offsetof)

  target_compile_definitions (seastar PUBLIC
    HAVE_DPDK)
endif ()

if (${SEASTAR_ALLOCATOR_PAGE_SIZE})
  target_compile_definitions (seastar PUBLIC
    SEASTAR_OVERRIDE_ALLOCATOR_PAGE_SIZE=${SEASTAR_ALLOCATOR_PAGE_SIZE})
endif ()

if (${SEASTAR_ENABLE_ALLOC_FAILURE_INJECTOR})
  target_compile_definitions (seastar PUBLIC
    SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION)
endif ()

##
## Code tests.
##

###
### have_lz4_compress_default
###

set (CMAKE_REQUIRED_INCLUDES ${Lz4_INCLUDE_DIRS})
set (CMAKE_REQUIRED_LIBRARIES ${Lz4_LIBRARIES})
check_symbol_exists (LZ4_compress_default lz4.h have_lz4_compress_default)

if (${have_lz4_compress_default})
  target_compile_definitions (seastar PUBLIC HAVE_LZ4_COMPRESS_DEFAULT)
endif ()

###
### have_asan_fiber_support
###

try_compile (have_asan_fiber_support
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake-tests/asan_fiber.cc
  LINK_LIBRARIES -fsanitize=address)

if (${have_asan_fiber_support})
  target_compile_definitions (seastar PUBLIC HAVE_ASAN_FIBER_SUPPORT)
endif ()

###
### have_sanitize_vptr_flag
###

# https://gcc.gnu.org/bugzilla/show_bug.cgi?id=67258
check_cxx_compiler_flag (-fsanitize=vptr have_sanitize_vptr_flag)

set (ENV{UBSAN_OPTIONS} "exitcode=1")
try_run (sanitize_vptr_run sanitize_vptr_compile
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake-tests/sanitize_vptr.cc
  CMAKE_FLAGS -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover")

# TODO: -fsanitize=vptr is broken even when the test passes.
if ((NOT "${have_sanitize_vptr_flag}") OR "${sanitize_vptr_run}" AND FALSE)
else ()
  if ("${CMAKE_BUILD_TYPE}" STREQUAL Debug)
    message (STATUS "-fsanitize=vptr is broken; disabling. Some debug-mode tests are bypassed.")
    target_compile_options (seastar PUBLIC -fno-sanitize=vptr)
  endif ()
endif ()

###
### visibility_flags_compile
###

# https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80947
try_compile (visiblity_flags_compile
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake-tests/visibility_flags.cc
  CMAKE_FLAGS -DCMAKE_CXX_FLAGS="-fvisibility=hidden -std=gnu++1y -Werror=attributes")

if (NOT "${visibility_flags_compile}")
  message (STATUS "Disabling -Wattributes due to https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80947")
  target_compile_options (seastar PUBLIC -Wno-attributes)
endif ()

##
## Warnings.
##

function (seastar_add_warning_if_supported warning)
  # GCC ignores the `-Wno` prefix even when the warning is not recognized, so strip it.
  string (REGEX REPLACE "^-Wno-" "-W" adjusted ${warning})
  string (REGEX REPLACE "^-W" "warning_" human_name_prefix ${adjusted})
  set (result "${human_name_prefix}_supported")
  check_cxx_compiler_flag (${adjusted} ${result})

  if ("${${result}}")
    target_compile_options (seastar PUBLIC ${warning})
  endif ()
endfunction ()

# Clang-only.
seastar_add_warning_if_supported (-Wno-mismatched-tags)

# Clang-only: moving a temporary object prevents copy elision.
seastar_add_warning_if_supported (-Wno-pessimizing-move)

# Clang-only: redundant move in return statement.
seastar_add_warning_if_supported (-Wno-redundant-move)

# Clang-only: 'x' overrides a member function but is not marked 'override'.
seastar_add_warning_if_supported (-Wno-inconsistent-missing-override)

# Clang-only: private field 'x' is not used.
seastar_add_warning_if_supported (-Wno-unused-private-field)

# Clang-only: unknown attribute 'x' ignored (x in this case is gnu::externally_visible).
seastar_add_warning_if_supported (-Wno-unknown-attributes)

# Clang-only: 'x' function 'y' declared in header file should be declared 'z'.
seastar_add_warning_if_supported (-Wno-unneeded-internal-declaration)

# Clang-only: inline function 'x' is not defined.
seastar_add_warning_if_supported (-Wno-undefined-inline)

# Clang-only: 'x' hides overloaded virtual functions.
seastar_add_warning_if_supported (-Wno-overloaded-virtual)

seastar_add_warning_if_supported (-Wno-maybe-uninitialized)

seastar_add_warning_if_supported (-Wno-sign-compare)

# GCC-only: Allow preprocessor warnings.
seastar_add_warning_if_supported (-Wno-error=cpp)

# Clang-only: Allow preprocessor warnings.
# TODO: The '#' symbol confuses CMake.
#seastar_add_warning_if_supported ("-Wno-error=#warning")

##
## Extra flags at the end.
##

target_compile_options (seastar PUBLIC "${SEASTAR_USER_CXXFLAGS}")

#
# Code generation.
#

add_subdirectory (proto)

function (seastar_ragel_generate file output_var)
  set (abs_file "${CMAKE_CURRENT_SOURCE_DIR}/${file}")

  get_filename_component (base_name ${file} NAME_WE)
  set (output_name "${base_name}.hh")

  add_custom_command (
    OUTPUT "${output_name}"
    MAIN_DEPENDENCY ${abs_file}
    COMMAND ${Ragel_EXECUTABLE} -G2 -o ${output_name} ${abs_file}
    # sed away a bug in ragel 7 that emits some extraneous _nfa* variables.
    COMMAND sed -i -e '1h\;2,$$H\;$$!d\;g' -re 's/static const char _nfa[^\;]*\;//g' ${output_name})

  set ("${output_var}" "${CMAKE_CURRENT_BINARY_DIR}/${output_name}" PARENT_SCOPE)
endfunction ()

function (seastar_swagger_generate file output_var)
  set (abs_file "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
  set (output_name "${file}.hh")
  set (output_path "${CMAKE_CURRENT_BINARY_DIR}/${output_name}")

  add_custom_command (
    OUTPUT ${output_path}
    MAIN_DEPENDENCY ${abs_file}
    COMMAND "${SEASTAR_SOURCE_DIR}/json/json2code.py" -f ${abs_file} -o ${output_path})

  set ("${output_var}" ${output_path} PARENT_SCOPE)
endfunction ()

add_subdirectory (http)

target_include_directories (seastar SYSTEM
  PUBLIC
    ${GNUTLS_INCLUDE_DIR}
    ${PROTOBUF_INCLUDE_DIRS})

target_include_directories (seastar PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR})

if (${SEASTAR_NABLE_HWLOC})
  target_include_directories (seastar SYSTEM
    PUBLIC
      ${LIBXML2_INCLUDE_DIRS})
endif ()

# Include -lgcc_s before -lunwind to work around for
# https://savannah.nongnu.org/bugs/?48486. See
# https://github.com/scylladb/scylla/issues/1725.

target_link_libraries (seastar PUBLIC
  -Wl,--no-as-needed
  -fvisibility=hidden)

target_link_libraries (seastar PUBLIC
  debug -fsanitize=address
  debug -fsanitize=leak
  debug -fsanitize=undefined
  -ldl
  -lgcc_s
  -lunwind
  ${CMAKE_THREAD_LIBS_INIT}
  ${GNUTLS_LIBRARIES}
  ${PROTOBUF_LIBRARIES}
  ${RT_LIBRARY}
  ${AIO_LIBRARY}
  Boost::boost # Headers only.
  Cryptopp::cryptopp
  Lz4::lz4
  Yaml-cpp::yaml-cpp
  fmt::fmt-header-only
  c-ares::cares)

set (boost_targets Boost::program_options Boost::filesystem Boost::thread)
string (REPLACE ";" "," joined_boost_targets "${boost_targets}")

if (${SEASTAR_LINK_STATIC_BOOST})
  target_link_libraries (seastar PUBLIC "-Wl,-Bstatic,${joined_boost_targets},-Wl,-Bdynamic")
else ()
  target_link_libraries (seastar PUBLIC "${boost_targets}")
endif ()

if (${SEASTAR_ENABLE_HWLOC})
  target_link_libraries (seastar PUBLIC
    ${NUMA_LIBRARY}
    ${PCIAccess_LIBRARY}
    ${LIBXML2_LIBRARIES}
    HWLoc::hwloc
    ZLIB::ZLIB)
endif ()

if (${SEASTAR_ENABLE_DPDK})
  set (dpdk_libraries
    -lrte_pmd_vmxnet3_uio
    -lrte_pmd_i40e
    -lrte_pmd_ixgbe
    -lrte_pmd_e1000
    -lrte_pmd_ring
    -lrte_pmd_bnxt
    -lrte_pmd_cxgbe
    -lrte_pmd_ena
    -lrte_pmd_enic
    -lrte_pmd_fm10k
    -lrte_pmd_nfp
    -lrte_pmd_qede
    -lrte_pmd_sfc_efx
    -lrte_hash
    -lrte_kvargs
    -lrte_mbuf
    -lrte_ethdev
    -lrte_eal
    -lrte_mempool
    -lrte_ring
    -lrte_cmdline
    -lrte_cfgfile)

  string (REPLACE ";" "," joined_dpdk_libraries "${dpdk_libraries}")

  target_link_libraries (seastar PUBLIC
    -L"${dpdk_binary_dir}/lib"
    -Wl,--whole-archive,${joined_dpdk_libraries},--no-whole-archive)
endif ()

target_link_libraries (seastar PUBLIC "${SEASTAR_USER_LDFLAGS}")

#
# Tests and applications
#
option(SEASTAR_WITH_TESTS "Enable building tests and apps" ON)
if(SEASTAR_WITH_TESTS)
  add_subdirectory(apps)
  enable_testing()
  add_subdirectory(tests)
endif(SEASTAR_WITH_TESTS)

#
# Documentation.
#

add_custom_target (tutorial-html
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMAND
    pandoc
    --self-contained
    --toc
    -c doc/template.css
    -V documentclas=report
    --chapters
    --number-sections
    -f markdown_github+pandoc_title_block
    --highlight-style tango
    doc/tutorial.md
    -o doc/tutorial.html)

add_custom_target (tutorial-pdf
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMAND
    pandoc
    -f markdown_github+pandoc_title_block
    --highlight-style tango
    --template=doc/template.tex
    doc/tutorial.md
    -o doc/tutorial.pdf)

#
# Source code navigation tools.
#

add_custom_target (cscope
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMAND sh -c find -name '*.[chS]' -o -name '*.cc' -o -name '*.hh' | cscope -bq -i-)

#
# Generate pkg-config specification.
#
# This is quite ugly, since we need to extract information from imported targets
# (like `Boost::program_options`) and resolve generator expessions ($<...>).
# CMake doesn't make it easy to do either.

set (imported_target_regex "[a-zA-Z0-9_\\-]+::[a-zA-ZA0-9_\\-]+")

set (pc_cflags "")
set (pc_libs "")

##
## Query an imported target for all its non-linking compiler options.
##
function (seastar_expand_imported_target_cflags target output)
  get_target_property (definitions ${target} INTERFACE_COMPILE_DEFINITIONS)
  get_target_property (options ${target} INTERFACE_COMPILE_OPTIONS)
  get_target_property (include_dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)

  set (cflags "")
  if (definitions)
    # All definitions need to be prefixed with -D.
    string (REGEX REPLACE "([^;]+)" -D\\1 definitions_args "${definitions}")
    list (APPEND cflags "${definitions_args}")
  endif ()
  if (options)
    list (APPEND cflags "${options}")
  endif ()
  if (include_dirs)
    list (REMOVE_DUPLICATES include_dirs)

    ##
    ## All directories need to be prefixed with -I. We only care about
    ## BUILD_INTERFACE directories, so we have to manually remove the
    ## INSTALL_INTERFACE ones.
    ##

    string (REGEX REPLACE "([^;]+)" -I\\1 include_dirs_args "${include_dirs}")
    string (REGEX REPLACE "-I\\$<INSTALL_INTERFACE:.+>" "" include_dirs_args_no_empty "${include_dirs_args}")
    list (APPEND cflags "${include_dirs_args_no_empty}")
  endif ()

  set ("${output}" "${cflags}" PARENT_SCOPE)
endfunction ()

function (seastar_expand_imported_target_libs target output)
  get_target_property (libs ${target} INTERFACE_LINK_LIBRARIES)
  set ("${output}" "${libs}" PARENT_SCOPE)
endfunction ()

##
## Start with the CMAKE_CXX cflags.
##

string (TOUPPER "${CMAKE_BUILD_TYPE}" build_type_uppercase)
string (REGEX REPLACE "[ ]+" ";" global_cflags "${CMAKE_CXX_FLAGS_${build_type_uppercase}}")
list (APPEND pc_cflags "${global_cflags}")

seastar_expand_imported_target_cflags (Seastar::seastar seastar_cflags)
list (APPEND pc_cflags "${seastar_cflags}")

##
## We expand the seastar library flags and process each one, looking for imported targets.
##

seastar_expand_imported_target_libs (Seastar::seastar seastar_libs_raw)
foreach (lib ${seastar_libs_raw})
  if (${lib} MATCHES ${imported_target_regex})
    ##
    ## For an imported target, expand its CFLAGS.
    ##

    seastar_expand_imported_target_cflags (${lib} lib_cflags)
    list (APPEND pc_cflags "${lib_cflags}")

    ##
    ## For non-header-only libraries, add the imported target's actual library file.
    ##

    get_target_property (lib_type ${lib} TYPE)
    if (NOT ("${lib_type}" STREQUAL INTERFACE_LIBRARY))
      set (lib_location $<TARGET_FILE:${lib}>)
      list (APPEND pc_libs "${lib_location}")
    endif ()
  else ()
    list (APPEND pc_libs "${lib}")
  endif ()
endforeach ()

list (REMOVE_DUPLICATES pc_cflags)

set (pc_file "${CMAKE_CURRENT_BINARY_DIR}/seastar.pc")

# At this point, both pc_libs and pc_cflags can contain generator expressions
# ($<...>). CMake gives us no way to evaluate these in code. They are evaluated
# by CMake internally at the build phase (but not the configure phase). Creating
# a target and echoing the strings forces their evaluation at the right time.
# This would not be possible with CONFIGURE_FILE.
#
# Note that seastar needs to be listed prior to other static libraries to
# prevent symbols from being omitted from them.
add_custom_target (pkg-config ALL
  COMMAND echo "Name: Seastar" > ${pc_file}
  COMMAND echo "URL: http://seastar-project.org/" >> ${pc_file}
  COMMAND echo "Description: Advanced C++ framework for high-performance server applications on modern hardware." >> ${pc_file}
  COMMAND echo "Version: 1.0" >> ${pc_file}
  COMMAND echo "Libs:" '-Wl,--whole-archive,$<TARGET_FILE:Seastar::seastar>,--no-whole-archive' '${pc_libs}' >> ${pc_file}
  COMMAND echo "Cflags:" '${pc_cflags}' >> ${pc_file})
