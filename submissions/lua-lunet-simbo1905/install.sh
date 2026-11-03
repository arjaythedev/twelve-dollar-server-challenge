#!/usr/bin/env bash
# Runtime dependencies for the pinned lunet release archive. Run once as root
# on a clean Ubuntu 24.04 box; re-running is safe.
set -euo pipefail

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl libluajit-5.1-2 libuv1 zlib1g libsqlite3-0 libsodium23

sodium_lib="$(ldconfig -p | grep -m1 libsodium.so | awk '{print $NF}')"
ln -sf "$sodium_lib" "$(dirname "$sodium_lib")/libsodium.so"
