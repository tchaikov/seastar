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
# Copyright (C) 2021 Kefu Chai <tchaikov@gmail.com>
#

find_package (PkgConfig REQUIRED)

if(spdk_FIND_COMPONENTS)
  if(NOT bdev IN_LIST spdk_FIND_COMPONENTS)
    list (APPEND spdk_FIND_COMPONENTS bdev)
  endif()
else()
  set(spdk_FIND_COMPONENTS
    bdev
    blobfs
    env_dpdk
    event
    ftl
    iscsi
    json
    jsonrpc
    log
    lvol
    nvme
    syslibs
    thread
    vhost)
endif()

include (FindPackageHandleStandardArgs)
set (spdk_INCLUDE_DIR)
set (spdk_LINK_DIRECTORIES)

set (_spdk_env_dpdk_deps archive)
set (_spdk_bdev_aio_deps aio)
set (_spdk_util_deps uuid)

foreach (component ${spdk_FIND_COMPONENTS})
  pkg_check_modules (spdk_${component} spdk_${component} QUIET)
  set (prefix spdk_${component}_STATIC)
  list (APPEND spdk_lib_vars ${prefix}_LIBRARIES)
  if (NOT spdk_${component}_FOUND)
    continue ()
  endif ()
  add_library (spdk::${component} INTERFACE IMPORTED)
  # add the dependencies of the linked SPDK libraries if any
  foreach (spdk_lib ${${prefix}_LIBRARIES})
    foreach (dep ${_${spdk_lib}_deps})
      find_package (${dep} QUIET REQUIRED)
      if (NOT ${dep}_FOUND)
        continue ()
      endif ()
      if (NOT ${${dep}_LIBRARIES} IN_LIST "${${prefix}_LIBRARIES}")
        list (APPEND ${prefix}_LIBRARIES ${${dep}_LIBRARIES})
      endif ()
    endforeach ()
  endforeach ()

  set_target_properties (spdk::${component}
    PROPERTIES
      INTERFACE_COMPILE_OPTIONS ${${prefix}_CFLAGS}
      INTERFACE_INCLUDE_DIRECTORIES ${${prefix}_INCLUDE_DIRS}
      INTERFACE_LINK_OPTIONS "-Wl,--whole-archive;${${prefix}_LDFLAGS};-Wl,--no-whole-archive"
      INTERFACE_LINK_LIBRARIES "${${prefix}_LIBRARIES}"
      INTERFACE_LINK_DIRECTORIES "${${prefix}_LIBRARY_DIRS}")
  if (NOT spdk_INCLUDE_DIR)
    set (spdk_INCLUDE_DIR ${${prefix}_INCLUDE_DIRS})
  endif ()
  if (NOT spdk_LINK_DIRECTORIES)
    set (spdk_LINK_DIRECTORIES ${${prefix}_LIBRARY_DIRS})
  endif ()
  list (APPEND spdk_link_opts "${${prefix}_LDFLAGS}")
  list (APPEND spdk_libs ${${prefix}_LIBRARIES})
endforeach ()

if (spdk_INCLUDE_DIR AND EXISTS "${spdk_INCLUDE_DIR}/spdk/version.h")
  foreach(ver "MAJOR" "MINOR" "PATCH")
    file(STRINGS "${spdk_INCLUDE_DIR}/spdk/version.h" spdk_VER_${ver}_LINE
      REGEX "^#define[ \t ]+SPDK_VERSION_${ver}[ \t]+[0-9]+$")
    string(REGEX REPLACE "^#define[ \t]+SPDK_VERSION_${ver}[ \t]+([0-9]+)$"
      "\\1" spdk_VERSION_${ver} "${spdk_VER_${ver}_LINE}")
    unset(${spdk_VER_${ver}_LINE})
  endforeach()
  set(spdk_VERSION_STRING
    "${spdk_VERSION_MAJOR}.${spdk_VERSION_MINOR}.${spdk_VERSION_PATCH}")
endif ()

list(REMOVE_DUPLICATES spdk_lib_vars)
find_package_handle_standard_args (spdk
  REQUIRED_VARS
    spdk_INCLUDE_DIR
    spdk_LINK_DIRECTORIES
    ${spdk_lib_vars}
  VERSION_VAR
    spdk_VERSION_STRING)

if (spdk_FOUND AND NOT (TARGET spdk::spdk))
  set (spdk_LIBRARIES ${spdk_libs})
  set (whole_archive_link_opts
    -Wl,--whole-archive -Wl,-Bstatic ${spdk_link_opts} -Wl,--no-whole-archive -Wl,-Bdynamic)
  add_library (spdk::spdk INTERFACE IMPORTED)
  set_target_properties (spdk::spdk
    PROPERTIES
      INTERFACE_COMPILE_OPTIONS "${spdk_PC_STATIC_bdev_CFLAGS}"
      INTERFACE_INCLUDE_DIRECTORIES "${spdk_INCLUDE_DIR}"
      INTERFACE_LINK_OPTIONS "${whole_archive_link_opts}"
      INTERFACE_LINK_LIBRARIES "${spdk_LIBRARIES}"
      INTERFACE_LINK_DIRECTORIES "${spdk_LINK_DIRECTORIES}")
endif ()
