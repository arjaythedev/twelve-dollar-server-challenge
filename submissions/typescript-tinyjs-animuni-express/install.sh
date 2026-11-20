#!/usr/bin/env bash
set -euo pipefail
apt-get update
apt-get install -y ca-certificates curl unzip
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL https://github.com/oven-sh/bun/releases/download/bun-v1.4.4/bun-linux-x64.zip -o "$tmp/bun.zip"
unzip -q "$tmp/bun.zip" -d "$tmp"
install -m 0755 "$tmp/bun-linux-x64/bun" /usr/local/bin/bun
