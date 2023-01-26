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
 * Copyright (C) 2023 ScyllaDB Ltd.
 */
export module seastar;
export import :core;
// #include <seastar/http/api_docs.hh>
// #include <seastar/http/common.hh>
// #include <seastar/http/exception.hh>
// #include <seastar/http/file_handler.hh>
// #include <seastar/http/function_handlers.hh>
// #include <seastar/http/handlers.hh>
// #include <seastar/http/httpd.hh>
// #include <seastar/http/json_path.hh>
// #include <seastar/http/matcher.hh>
// #include <seastar/http/matchrules.hh>
// #include <seastar/http/mime_types.hh>
// #include <seastar/http/reply.hh>
// #include <seastar/http/request.hh>
// #include <seastar/http/routes.hh>
// #include <seastar/http/short_streams.hh>
// #include <seastar/http/transformers.hh>
// #include <seastar/http/client.hh>
// #include <seastar/json/formatter.hh>
// #include <seastar/json/json_elements.hh>
// #include <seastar/net/api.hh>
// #include <seastar/net/arp.hh>
// #include <seastar/net/byteorder.hh>
// #include <seastar/net/config.hh>
// #include <seastar/net/const.hh>
// #include <seastar/net/dhcp.hh>
// #include <seastar/net/dns.hh>
// #include <seastar/net/dpdk.hh>
// #include <seastar/net/ethernet.hh>
// #include <seastar/net/inet_address.hh>
// #include <seastar/net/ip.hh>
// #include <seastar/net/ip_checksum.hh>
// #include <seastar/net/native-stack.hh>
// #include <seastar/net/net.hh>
// #include <seastar/net/packet-data-source.hh>
// #include <seastar/net/packet-util.hh>
// #include <seastar/net/packet.hh>
// #include <seastar/net/posix-stack.hh>
// #include <seastar/net/proxy.hh>
// #include <seastar/net/socket_defs.hh>
// #include <seastar/net/stack.hh>
// #include <seastar/net/tcp-stack.hh>
// #include <seastar/net/tcp.hh>
// #include <seastar/net/tls.hh>
// #include <seastar/net/toeplitz.hh>
// #include <seastar/net/udp.hh>
// #include <seastar/net/unix_address.hh>
// #include <seastar/net/virtio-interface.hh>
// #include <seastar/net/virtio.hh>
// #include <seastar/rpc/lz4_compressor.hh>
// #include <seastar/rpc/lz4_fragmented_compressor.hh>
// #include <seastar/rpc/multi_algo_compressor_factory.hh>
// #include <seastar/rpc/rpc.hh>
// #include <seastar/rpc/rpc_impl.hh>
// #include <seastar/rpc/rpc_types.hh>
// #include <seastar/util/alloc_failure_injector.hh>
// #include <seastar/util/backtrace.hh>
// #include <seastar/util/concepts.hh>
// #include <seastar/util/bool_class.hh>
// #include <seastar/util/conversions.hh>
// #include <seastar/util/defer.hh>
// #include <seastar/util/eclipse.hh>
// #include <seastar/util/function_input_iterator.hh>
// #include <seastar/util/gcc6-concepts.hh>
// #include <seastar/util/indirect.hh>
// #include <seastar/util/is_smart_ptr.hh>
// #include <seastar/util/lazy.hh>
// #include <seastar/util/log-cli.hh>
// #include <seastar/util/log-impl.hh>
// #include <seastar/util/log.hh>
// #include <seastar/util/noncopyable_function.hh>
// #include <seastar/util/optimized_optional.hh>
// #include <seastar/util/print_safe.hh>
// #include <seastar/util/process.hh>
// #include <seastar/util/program-options.hh>
// #include <seastar/util/read_first_line.hh>
// #include <seastar/util/reference_wrapper.hh>
// #include <seastar/util/spinlock.hh>
// #include <seastar/util/std-compat.hh>
// #include <seastar/util/transform_iterator.hh>
// #include <seastar/util/tuple_utils.hh>
// #include <seastar/util/variant_utils.hh>
// #include <seastar/util/closeable.hh>
// #include <seastar/util/source_location-compat.hh>
// #include <seastar/util/short_streams.hh>
// #include <seastar/websocket/server.hh>
