#!/usr/bin/env bash
# Installs the Zig toolchain (official tarball, checksum verified). Run once as root on a clean Ubuntu 24.04.
set -euo pipefail
ZIG_VERSION=0.17.0
ZIG_SHA256=1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026
ZIG_URL="https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz"

if [ -x /opt/zig/zig ] && [ "$(/opt/zig/zig version)" = "$ZIG_VERSION" ]; then
  echo "zig $ZIG_VERSION already installed"
else
  export DEBIAN_FRONTEND=noninteractive
  apt-get install -y -q curl xz-utils ca-certificates unzip build-essential libsqlite3-dev >/dev/null
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  curl -fsSL --retry 5 -o "$tmp/zig.tar.xz" "$ZIG_URL"
  echo "$ZIG_SHA256  $tmp/zig.tar.xz" | sha256sum -c -
  rm -rf /opt/zig
  mkdir -p /opt/zig
  tar -xJf "$tmp/zig.tar.xz" -C /opt/zig --strip-components=1
fi
ln -sf /opt/zig/zig /usr/local/bin/zig
zig version
