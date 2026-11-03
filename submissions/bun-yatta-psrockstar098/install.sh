#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Installs a pinned Bun version
# (official release zip, SHA-256 verified) and nothing else: the server needs
# no system packages beyond curl/unzip/ca-certificates, and bun:sqlite is
# built into Bun.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl unzip
BUN_VERSION=1.4.4
asset=bun-linux-x64-baseline
sha=e0446eb55995772eb9894bebae9a6be368d985a8b9271ee124486f9946eedd76
if grep -qw avx2 /proc/cpuinfo; then
  asset=bun-linux-x64
  sha=bf8ccbfb2b0b80159e0f9a6d15c55317add8077534dbc6130a3edb52fccb395c
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL "https://github.com/oven-sh/bun/releases/download/bun-v${BUN_VERSION}/$asset.zip" -o "$tmp/bun.zip"
echo "$sha  $tmp/bun.zip" | sha256sum -c -
unzip -q "$tmp/bun.zip" -d "$tmp"
install -m 755 "$tmp/$asset/bun" /usr/local/bin/bun
bun --version
