#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Zig 0.15.2 (official tarball, SHA-256 pinned) plus the
# tools build.sh uses to fetch SQLite. Zig compiles both the app and SQLite, so no gcc is needed.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends curl ca-certificates unzip xz-utils
ZIG=zig-x86_64-linux-0.15.2
SHA=02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239
curl -fsSL -o /tmp/$ZIG.tar.xz https://ziglang.org/download/0.15.2/$ZIG.tar.xz
echo "$SHA  /tmp/$ZIG.tar.xz" | sha256sum -c -
rm -rf /opt/$ZIG && tar -C /opt -xJf /tmp/$ZIG.tar.xz && rm /tmp/$ZIG.tar.xz
ln -sf /opt/$ZIG/zig /usr/local/bin/zig
zig version
