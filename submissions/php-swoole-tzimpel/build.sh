#!/usr/bin/env bash
# Builds the app as a normal user: compiles a pinned Swoole release into build/swoole.so.
set -euo pipefail
cd "$(dirname "$0")"
SWOOLE_VERSION=6.2.3
SWOOLE_SHA256=dde8d2a4a6b5c5cd418aedd8561760baad59767a2f8a963b2c0e9eb9c86f4c8d

[ -f build/swoole.so ] && [ "$(cat build/version 2>/dev/null)" = "$SWOOLE_VERSION" ] && exit 0
rm -rf build && mkdir -p build/src
curl -fsSL "https://github.com/swoole/swoole-src/archive/refs/tags/v$SWOOLE_VERSION.tar.gz" -o build/swoole.tar.gz
echo "$SWOOLE_SHA256  build/swoole.tar.gz" | sha256sum -c -
tar -xzf build/swoole.tar.gz -C build/src --strip-components=1
# Memory patch: once a request is answered, Swoole keeps a fresh 64 KiB receive buffer (from PHP's
# heap) on the keep-alive connection for its next request. At 40k mostly idle connections that is
# 2.5 GB of heap (memory_limit) and ~16 KiB RSS each. Free it instead (both the no-body and the
# body branch); the next request on the connection allocates a new one.
perl -0pi -e 's/(\} else \{\n\s+port->destroy_http_request\(conn\);\n\s+)buffer->clear\(\);(\n\s+return SW_OK;)/$1delete _socket->recv_buffer; _socket->recv_buffer = nullptr; \/\/ patched$2/; s/if \(_socket->recv_buffer && _socket->recv_buffer->size > SW_BUFFER_SIZE_BIG \* 2\) \{(\n\s+delete _socket->recv_buffer;\n\s+_socket->recv_buffer = nullptr;\n\s+\} else \{\n\s+buffer->clear\(\);\n\s+\})/if (_socket->recv_buffer) { \/\/ patched$1/' build/src/src/server/port.cc
[ "$(grep -c '// patched' build/src/src/server/port.cc)" = 2 ] || { echo "swoole memory patch did not apply"; exit 1; }
(
  cd build/src
  phpize >/dev/null
  # Only the HTTP server is used: no OpenSSL, cURL, c-ares, io_uring or database hooks.
  CFLAGS="-O2" ./configure --quiet
  make -j"$(( $(nproc) < 2 ? $(nproc) : 2 ))" >/dev/null  # each g++ needs ~0.5-1 GB
)
cp build/src/modules/swoole.so build/swoole.so
echo "$SWOOLE_VERSION" > build/version
rm -rf build/src build/swoole.tar.gz
