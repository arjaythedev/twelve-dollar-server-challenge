#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Zig 0.17.0 (official tarball, SHA-256 pinned), gcc (for
# SQLite's profile-guided build) and the tools build.sh uses to fetch SQLite.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip xz-utils
ZIG=zig-$(uname -m)-linux-0.17.0
case $ZIG in  # the benchmark box is x86_64; aarch64 is for local testing
  zig-x86_64-*) SHA=1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026 ;;
  zig-aarch64-*) SHA=9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8 ;;
esac
curl -fsSL -o /tmp/$ZIG.tar.xz https://ziglang.org/download/0.17.0/$ZIG.tar.xz
echo "$SHA  /tmp/$ZIG.tar.xz" | sha256sum -c -
rm -rf /opt/$ZIG && tar -C /opt -xJf /tmp/$ZIG.tar.xz && rm /tmp/$ZIG.tar.xz
ln -sf /opt/$ZIG/zig /usr/local/bin/zig
zig version
