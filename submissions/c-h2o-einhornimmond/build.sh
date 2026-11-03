#!/usr/bin/env bash
# Builds bin/server as a normal user. zig fetches the dependencies pinned in build.zig.zon
# (each with its hash) into the user's cache and compiles all of them from source: h2o,
# LibreSSL, zlib, SQLite, libsodium and arnm. A first build takes several minutes on one core.
set -euo pipefail
cd "$(dirname "$0")"

ZIG="${ZIG:-/opt/zig-0.15.2/zig}"
"$ZIG" build --fetch
"$ZIG" build -Doptimize=ReleaseFast --prefix .
echo "built bin/server"
