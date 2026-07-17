#!/bin/bash
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License"). See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership. You may not use this file except in compliance with the License.
#
# QUIC Interop Runner endpoint entrypoint for the Seastar HTTP/3 server.
#
# The base image (martenseemann/quic-network-simulator-endpoint) provides
# /setup.sh and /wait-for-it.sh and wires up the ns-3 network simulator.
set -e

# Set up the routes for the network simulator.
/setup.sh

# The runner passes the scenario via $TESTCASE, extra flags via
# $SERVER_PARAMS, and a qlog directory via $QLOGDIR. Certificates are at
# /certs, content is served from /www, on UDP port 443.

if [ "$ROLE" != "server" ]; then
    echo "Only ROLE=server is supported"
    exit 127
fi

case "$TESTCASE" in
    handshake|transfer|http3|multiplexing|retry|resumption|zerortt|chacha20|keyupdate)
        ;;
    *)
        echo "unsupported testcase: $TESTCASE"
        exit 127
        ;;
esac

exec /quic_interop \
    --smp 1 \
    --cert /certs/cert.pem \
    --key /certs/priv.key \
    --docroot /www \
    --port 443 \
    $SERVER_PARAMS \
    &> /logs/log.txt
