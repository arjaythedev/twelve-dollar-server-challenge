#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Zig 0.15.2 (official tarball, SHA-256 pinned), gcc (for
# SQLite's profile-guided build) and the tools build.sh uses to fetch SQLite.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip xz-utils
ZIG=zig-$(uname -m)-linux-0.15.2
case $ZIG in  # the benchmark box is x86_64; aarch64 is for local testing
  zig-x86_64-*) SHA=02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239 ;;
  zig-aarch64-*) SHA=958ed7d1e00d0ea76590d27666efbf7a932281b3d7ba0c6b01b0ff26498f667f ;;
esac
curl -fsSL -o /tmp/$ZIG.tar.xz https://ziglang.org/download/0.15.2/$ZIG.tar.xz
echo "$SHA  /tmp/$ZIG.tar.xz" | sha256sum -c -
rm -rf /opt/$ZIG && tar -C /opt -xJf /tmp/$ZIG.tar.xz && rm /tmp/$ZIG.tar.xz
ln -sf /opt/$ZIG/zig /usr/local/bin/zig
zig version
