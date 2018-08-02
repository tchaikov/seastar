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

#
# Useful definitions for `cmake -E env`.
#

set (amended_PATH PATH=${Cooking_INGREDIENTS_DIR}/bin:$ENV{PATH})
set (PKG_CONFIG_PATH PKG_CONFIG_PATH=${Cooking_INGREDIENTS_DIR}/lib/pkgconfig)

#
# Some Autotools ingredients need this information because they don't use pkgconfig.
#

set (autotools_ingredients_flags
  CFLAGS=-I${Cooking_INGREDIENTS_DIR}/include
  CXXFLAGS=-I${Cooking_INGREDIENTS_DIR}/include
  LDFLAGS=-L${Cooking_INGREDIENTS_DIR}/lib)

#
# Some Autotools projects amend the info file instead of making a package-specific one.
# This doesn't play nicely with GNU Stow.
#
# Just append the name of the ingredient, like
#
#     ${info_dir}/gmp
#

set (info_dir --infodir=<INSTALL_DIR>/share/info)

#
# Build-concurrency.
#

cmake_host_system_information (
  RESULT build_concurrency_factor
  QUERY NUMBER_OF_LOGICAL_CORES)

set (make_command make -j ${build_concurrency_factor})

#
# All the ingredients.
#

##
## Dependencies of dependencies of dependencies.
##

cooking_ingredient (gmp
  URL https://gmplib.org/download/gmp/gmp-6.1.2.tar.bz2
  URL_MD5 8ddbb26dc3bd4e2302984debba1406a5
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR> ${info_dir}/gmp
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

##
## Dependencies of dependencies.
##

cooking_ingredient (colm
  URL http://www.colm.net/files/colm/colm-0.13.0.6.tar.gz
  URL_MD5 16aaf566cbcfe9a06154e094638ac709
  # This is upsetting.
  BUILD_IN_SOURCE YES
  CONFIGURE_COMMAND ./configure --prefix=<INSTALL_DIR>
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

cooking_ingredient (libpciaccess
  URL https://www.x.org/releases/individual/lib/libpciaccess-0.13.4.tar.gz
  URL_MD5 cc1fad87da60682af1d5fa43a5da45a4
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR>
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

cooking_ingredient (nettle
  DEPENDS ingredient_gmp
  URL https://ftp.gnu.org/gnu/nettle/nettle-3.4.tar.gz
  URL_MD5 dc0f13028264992f58e67b4e8915f53d
  CONFIGURE_COMMAND
    <SOURCE_DIR>/configure
    --prefix=<INSTALL_DIR>
    --srcdir=<SOURCE_DIR>
    --libdir=<INSTALL_DIR>/lib
    ${info_dir}/nettle
    ${autotools_ingredients_flags}
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

# Also a direct dependency of Seastar.
cooking_ingredient (numactl
  GIT_REPOSITORY https://github.com/numactl/numactl.git
  GIT_TAG f1849d0d7a50a6b9d507b7cc6164b04aae948d54
  PATCH_COMMAND ./autogen.sh
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR>
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

cooking_ingredient (zlib
  URL https://zlib.net/zlib-1.2.11.tar.gz
  URL_MD5 1c9f62f0778697a09d36121ead88e08e
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --64
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

##
## Private and private/public dependencies.
##

cooking_ingredient (Boost
  # The 1.67.0 release has a bug in Boost Lockfree around a missing header.
  URL https://dl.bintray.com/boostorg/release/1.64.0/source/boost_1_64_0.tar.gz
  URL_MD5 319c6ffbbeccc366f14bb68767a6db79
  PATCH_COMMAND
    ./bootstrap.sh
    --prefix=<INSTALL_DIR>
    --with-libraries=atomic,chrono,date_time,filesystem,program_options,system,test,thread
  CONFIGURE_COMMAND <DISABLE>
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND
    ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
    ./b2
    -j ${build_concurrency_factor}
    --layout=system
    --build-dir=<BINARY_DIR>
    install
    variant=debug
    link=shared
    threading=multi)

cooking_ingredient (GnuTLS
  DEPENDS
    ingredient_gmp
    ingredient_nettle
  URL https://www.gnupg.org/ftp/gcrypt/gnutls/v3.5/gnutls-3.5.18.tar.xz
  URL_MD5 c2d93d305ecbc55939bc2a8ed4a76a3d
  CONFIGURE_COMMAND
   ${CMAKE_COMMAND} -E env ${PKG_CONFIG_PATH}
    <SOURCE_DIR>/configure
    --prefix=<INSTALL_DIR>
    --srcdir=<SOURCE_DIR>
    --with-included-unistring
    --with-included-libtasn1
    --without-p11-kit
    # https://lists.gnupg.org/pipermail/gnutls-help/2016-February/004085.html
    --disable-non-suiteb-curves
    --disable-doc
    ${autotools_ingredients_flags}
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

cooking_ingredient (Protobuf
  DEPENDS ingredient_zlib
  URL https://github.com/google/protobuf/releases/download/v3.4.1/protobuf-cpp-3.4.1.tar.gz
  URL_MD5 74446d310ce79cf20bab3ffd0e8f8f8f
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR>
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

cooking_ingredient (hwloc
  DEPENDS
    ingredient_numactl
    ingredient_libpciaccess
  URL https://download.open-mpi.org/release/hwloc/v1.11/hwloc-1.11.5.tar.gz
  URL_MD5 8f5fe6a9be2eb478409ad5e640b2d3ba
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR>
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

cooking_ingredient (ragel
  DEPENDS ingredient_colm
  URL http://www.colm.net/files/ragel/ragel-7.0.0.11.tar.gz
  URL_MD5 ad58c8f1ac5def94d9c7f4395a03606c
  # This is upsetting.
  BUILD_IN_SOURCE YES
  CONFIGURE_COMMAND
    ${CMAKE_COMMAND} -E env ${amended_PATH}
    ./configure
    --prefix=<INSTALL_DIR>
    # This is even more upsetting.
    ${autotools_ingredients_flags}
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

cooking_ingredient (lksctp-tools
  URL https://sourceforge.net/projects/lksctp/files/lksctp-tools/lksctp-tools-1.0.16.tar.gz
  URL_MD5 708bb0b5a6806ad6e8d13c55b067518e
  PATCH_COMMAND ./bootstrap
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR>
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} install)

cooking_ingredient (yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG b57efe94e7d445713c29f863adb8c23438eaa217
  CMAKE_ARGS
    -DYAML_CPP_BUILD_TESTS=OFF
    -DBUILD_SHARED_LIBS=ON)

##
## Public dependencies.
##

cooking_ingredient (c-ares
  GIT_REPOSITORY https://github.com/c-ares/c-ares.git
  GIT_TAG fd6124c74da0801f23f9d324559d8b66fb83f533)

cooking_ingredient (cryptopp
  GIT_REPOSITORY https://github.com/weidai11/cryptopp.git
  GIT_TAG c621ce053298fafc1e59191079c33acd76045c26
  CMAKE_ARGS
    -DCMAKE_INSTALL_LIBDIR=<INSTALL_DIR>/lib
    -DBUILD_TESTING=OFF)

set (dpdk_quadruple ${CMAKE_SYSTEM_PROCESSOR}-native-linuxapp-gcc)

set (dpdk_args
  EXTRA_CFLAGS=-Wno-error
  O=<BINARY_DIR>
  DESTDIR=<INSTALL_DIR>
  T=${dpdk_quadruple})

cooking_ingredient (dpdk
  GIT_REPOSITORY https://github.com/scylladb/dpdk.git
  GIT_TAG a1774652fbbb1fe7c0ff392d5e66de60a0154df6
  CONFIGURE_COMMAND
    COMMAND
      ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
      make ${dpdk_args} config
    COMMAND
      ${CMAKE_COMMAND}
      -DSeastar_DPDK_CONFIG_FILE_IN=<BINARY_DIR>/.config
      -DSeastar_DPDK_CONFIG_FILE_CHANGES=${CMAKE_CURRENT_LIST_DIR}/dpdk_config
      -DSeastar_DPDK_CONFIG_FILE_OUT=<BINARY_DIR>/${dpdk_quadruple}/.config
      -P ${CMAKE_CURRENT_LIST_DIR}/dpdk_configure.cmake
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND
    ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
    ${make_command} ${dpdk_args} install)

cooking_ingredient (fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG 398343897f98b88ade80bbebdcbe82a36c65a980
  CMAKE_ARGS
    -DBUILD_SHARED_LIBS=ON
    -DFMT_DOC=OFF
    -DFMT_TEST=OFF)

cooking_ingredient (lz4
  GIT_REPOSITORY https://github.com/lz4/lz4.git
  GIT_TAG b3692db46d2b23a7c0af2d5e69988c94f126e10a
  # This is upsetting.
  BUILD_IN_SOURCE ON
  CONFIGURE_COMMAND <DISABLE>
  BUILD_COMMAND <DISABLE>
  INSTALL_COMMAND ${make_command} PREFIX=<INSTALL_DIR> install)
