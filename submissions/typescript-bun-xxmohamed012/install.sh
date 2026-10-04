#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs Bun 1.4.2 from the official release zip (checksum pinned).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl unzip
VERSION=1.4.2
# The default build needs AVX2; the baseline build runs on any x86_64 CPU.
if grep -qw avx2 /proc/cpuinfo; then
  ASSET=bun-linux-x64; SHA256=36368faef7527875d5ffa52e53cd48021741f2a83eb6208a8dd64068d422a913
else
  ASSET=bun-linux-x64-baseline; SHA256=c678040f14fe0440eb839d37cbd0ce4c051a32da72806ac97de6a6aab6bf728f
fi
tmp="$(mktemp -d)"
curl -fsSL -o "$tmp/bun.zip" "https://github.com/oven-sh/bun/releases/download/bun-v$VERSION/$ASSET.zip"
echo "$SHA256  $tmp/bun.zip" | sha256sum -c -
unzip -q "$tmp/bun.zip" -d "$tmp"
install -m 755 "$tmp/$ASSET/bun" /usr/local/bin/bun
rm -rf "$tmp"
bun --version
