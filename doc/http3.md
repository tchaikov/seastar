# QUIC and HTTP/3 in Seastar (experimental)

Seastar provides an experimental QUIC transport (RFC 9000) and an HTTP/3
layer (RFC 9114), built on the [ngtcp2](https://github.com/ngtcp2/ngtcp2)
and [nghttp3](https://github.com/ngtcp2/nghttp3) libraries. The public API
lives in the experimental namespaces `seastar::net::quic` and
`seastar::experimental::http3` and may change without notice.

## Why ngtcp2 / nghttp3

QUIC is complex enough that a from-scratch implementation was never on the
table for an experimental feature; the choice was which existing library
to embed. The main contenders were:

- **ngtcp2 + nghttp3** (MIT license) — a transport-only QUIC engine with a
  cleanly separated HTTP/3 library. It does no I/O, timers, or event loop
  of its own: the embedder drives it (feed datagrams in, pull packets
  out, arm one timer), which maps directly onto Seastar's per-shard
  reactor and lets us keep sockets, sharding, and buffer ownership on the
  Seastar side. TLS is pluggable (GnuTLS or OpenSSL), matching Seastar's
  existing runtime `--crypto-provider` selection.
- **quiche** (Cloudflare, BSD) — also I/O-free, but Rust; embedding it
  would add a Rust toolchain and a C-ABI boundary to Seastar's build.
- **lsquic** (LiteSpeed, MIT) and **msquic** (Microsoft, MIT) — larger,
  and more opinionated about their own event/callback models, which fits
  Seastar's reactor less naturally.
- **picoquic** (BSD) — research-oriented, less production hardening.

ngtcp2/nghttp3 won on being C, I/O-free (so the reactor stays in charge),
permissively licensed (MIT), and TLS-backend-agnostic. They are pulled in
from local checkouts and built as static libraries (see below).

## Building

The feature is gated behind a CMake option and requires
**ngtcp2 >= 1.23.0** and **nghttp3 >= 1.12.0** (older releases lack the
`*2` accessor APIs the transport uses; `find_package` rejects them at
configure time). Both ship CMake config packages, so a
new-enough system install is used as-is:

```sh
./configure.py --mode=release --enable-experimental-quic
ninja -C build/release
```

Distro packages are often too old — Debian trixie, for instance, ships
ngtcp2 1.11 / nghttp3 1.8. When the system libraries are missing or too
old, `--cook` builds pinned upstream releases (ngtcp2 1.24.0 /
nghttp3 1.17.0) from source as static libraries via cmake-cooking:

```sh
./configure.py --mode=release --enable-experimental-quic \
    --cook ngtcp2 --cook nghttp3
ninja -C build/release
```

or directly with CMake, pointing `CMAKE_PREFIX_PATH` at an ngtcp2 /
nghttp3 install:

```sh
cmake -S . -B build -DSeastar_EXPERIMENTAL_QUIC=ON
```

### Crypto provider

QUIC uses TLS 1.3 as a key-exchange provider (not as a record layer), via
ngtcp2's crypto helper libraries. Exactly one helper can be linked into a
binary, so the provider is fixed at build time with
`-DSeastar_QUIC_CRYPTO_PROVIDER=auto|gnutls|openssl` (default `auto`:
GnuTLS if available, otherwise OpenSSL). The **runtime** provider
(`--crypto-provider`) must match; OpenSSL needs version 3.5+ (native QUIC
TLS API) and GnuTLS 3.7.5+.

## QUIC transport API (`seastar::net::quic`)

```cpp
#include <seastar/net/quic/quic.hh>

// Server: bind one instance per shard (SO_REUSEPORT + CID routing).
auto server = quic::listen(addr, server_credentials, {});
quic::connection conn = co_await server.accept();

// Client:
quic::connect_options opts{.alpn_protocols = {"myproto"}};
quic::connection conn = co_await quic::connect(addr, client_credentials, opts);

// Multi-stream: open/accept bidirectional or unidirectional streams,
// each exposing input_stream/output_stream.
quic::stream s = co_await conn.open_stream(quic::stream_kind::bidirectional);
auto out = s.output();
co_await out.write("hello");
co_await out.close();

// Client connection migration (RFC 9000 §9):
co_await conn.migrate(new_local_address);
```

Highlights: sharding via SO_REUSEPORT with a shard id encoded in
server-chosen connection IDs (stray datagrams are forwarded to the owning
shard); UDP GSO/GRO on the POSIX stack; DATAGRAM frames (RFC 9221);
address-validation Retry, version negotiation, stateless reset; and TLS
session-ticket / NEW_TOKEN hooks for resumption. Errors use dedicated
`std::error_category` types; the synchronous entry points return
`std::expected`, while futures fail with `std::system_error`.

QUIC I/O uses kernel UDP sockets directly and is supported with the
default (posix) network stack only.

## HTTP/3 API (`seastar::experimental::http3`)

The server reuses `httpd::routes` and `http::request` / `http::reply`, so
route handlers written for `httpd::http_server` work unchanged:

```cpp
#include <seastar/http3/server.hh>

http3::http3_server server;
server._routes.put(httpd::GET, "/hello",
    new httpd::function_handler([] (httpd::const_req) {
        return sstring("world");
    }, "txt"));
co_await server.listen(addr, server_credentials);  // "h3" ALPN added automatically
```

The client multiplexes requests as concurrent bidirectional streams over
one QUIC connection, with an `http::client`-style `make_request`:

```cpp
#include <seastar/http3/client.hh>

http3::client client(addr, client_credentials, "example.com");
http::request req; req._method = "GET"; req._url = "/hello";
co_await client.make_request(std::move(req),
    [] (const http::reply& rep, input_stream<char>&& body) -> future<> {
        auto data = co_await util::read_entire_stream_contiguous(body);
        // ...
    });
```

Use `http3_server_control` (mirroring `httpd::http_server_control`) to run
a sharded server.

## Tools

- `apps/quic_interop` — a
  [QUIC Interop Runner](https://github.com/quic-interop/quic-interop-runner)
  server endpoint that serves files over HTTP/3 (see
  `apps/quic_interop/interop/` for the Dockerfile and `run_endpoint.sh`).
- `apps/h3wreck` — a seawreck-style HTTP/3 load generator reporting
  throughput and latency percentiles.
- `demos/http3_server_demo.cc` — a minimal HTTP/3 server.

## Tests

`tests/unit/quic_test.cc` covers the transport (handshake, streams, flow
control, close/idle, datagrams, Retry, migration, resumption) and
`tests/unit/http3_test.cc` covers HTTP/3 (GET, 404, streamed bodies,
concurrent requests). Both run under each crypto provider and are clean
under AddressSanitizer/LeakSanitizer.
