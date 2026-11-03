#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs Bun 1.4.3 from the official release zip (checksum pinned).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl unzip
VERSION=1.4.3
# The default build needs AVX2; the baseline build runs on any x86_64 CPU.
if grep -qw avx2 /proc/cpuinfo; then
  ASSET=bun-linux-x64; SHA256=3e730528d89775f03f87f05c06787c4df4099d0a37f3f252e7b829cb23816e18
else
  ASSET=bun-linux-x64-baseline; SHA256=1fc2edac843102909e3a1be1d8d9802cc6071cf074e67e88231f4ffe0f8b397b
fi
tmp="$(mktemp -d)"
curl -fsSL -o "$tmp/bun.zip" "https://github.com/oven-sh/bun/releases/download/bun-v$VERSION/$ASSET.zip"
echo "$SHA256  $tmp/bun.zip" | sha256sum -c -
unzip -q "$tmp/bun.zip" -d "$tmp"
install -m 755 "$tmp/$ASSET/bun" /usr/local/bin/bun
rm -rf "$tmp"
bun --version
