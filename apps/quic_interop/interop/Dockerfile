# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License"). See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership. You may not use this file except in compliance with the License.
#
# QUIC Interop Runner endpoint image for the Seastar HTTP/3 server.
#
# Build from the repository root:
#   docker build -f apps/quic_interop/interop/Dockerfile -t seastar-h3-interop .
#
# NOTE: a cold Seastar build pulls a large toolchain and takes tens of
# minutes; for iteration, build against a pinned prebuilt build environment
# image and mount a ccache directory.

FROM ubuntu:24.04 AS build

ARG NGTCP2_DIR=/src/ngtcp2
ARG NGHTTP3_DIR=/src/nghttp3

RUN apt-get update && apt-get install -y --no-install-recommends \
        git build-essential cmake ninja-build pkg-config ragel \
        python3 ca-certificates && rm -rf /var/lib/apt/lists/*

COPY . /src/seastar
# ngtcp2 and nghttp3 checkouts are expected under /src (bind-mount or COPY).
COPY --from=ngtcp2 . ${NGTCP2_DIR}
COPY --from=nghttp3 . ${NGHTTP3_DIR}

WORKDIR /src/seastar
RUN ./install-dependencies.sh
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_CXX_STANDARD=23 \
        -DSeastar_EXPERIMENTAL_QUIC=ON \
        -DSeastar_NGTCP2_SOURCE_DIR=${NGTCP2_DIR} \
        -DSeastar_NGHTTP3_SOURCE_DIR=${NGHTTP3_DIR} \
        -DSeastar_TESTING=OFF -DSeastar_DEMOS=OFF -DSeastar_DPDK=OFF && \
    ninja -C build apps/quic_interop/quic_interop && \
    strip build/apps/quic_interop/quic_interop

FROM martenseemann/quic-network-simulator-endpoint:latest
COPY --from=build /src/seastar/build/apps/quic_interop/quic_interop /quic_interop
# Copy the runtime shared libraries the binary needs.
COPY --from=build /usr/lib/x86_64-linux-gnu/libgnutls.so.30 /usr/lib/x86_64-linux-gnu/
COPY apps/quic_interop/interop/run_endpoint.sh /run_endpoint.sh
RUN chmod +x /run_endpoint.sh
ENTRYPOINT ["/run_endpoint.sh"]
