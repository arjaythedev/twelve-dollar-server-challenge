#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the zig toolchain, and the C library headers it
# compiles against. Everything else is a pinned dependency that build.sh fetches.
set -euo pipefail

ZIG_VERSION=0.15.2
ZIG_SHA256=02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239
ZIG_TARBALL="zig-x86_64-linux-${ZIG_VERSION}.tar.xz"

apt-get update
apt-get install -y curl xz-utils ca-certificates libc6-dev

cd "$(mktemp -d)"
curl -fsSL --retry 3 -o "$ZIG_TARBALL" "https://ziglang.org/download/${ZIG_VERSION}/${ZIG_TARBALL}"
echo "${ZIG_SHA256}  ${ZIG_TARBALL}" | sha256sum -c --quiet -
rm -rf "/opt/zig-${ZIG_VERSION}"
mkdir -p "/opt/zig-${ZIG_VERSION}"
tar -xJf "$ZIG_TARBALL" -C "/opt/zig-${ZIG_VERSION}" --strip-components=1
