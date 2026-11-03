#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl unzip
asset=bun-linux-x64-baseline
sha=c678040f14fe0440eb839d37cbd0ce4c051a32da72806ac97de6a6aab6bf728f
if grep -qw avx2 /proc/cpuinfo; then
  asset=bun-linux-x64
  sha=36368faef7527875d5ffa52e53cd48021741f2a83eb6208a8dd64068d422a913
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL "https://github.com/oven-sh/bun/releases/download/bun-v1.4.2/$asset.zip" -o "$tmp/bun.zip"
echo "$sha  $tmp/bun.zip" | sha256sum -c -
unzip -q "$tmp/bun.zip" -d "$tmp"
install -m 755 "$tmp/$asset/bun" /usr/local/bin/bun
