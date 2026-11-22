#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Installs the pinned Bun runtime.
set -euo pipefail
BUN_VERSION="1.3.14"
apt-get update && apt-get install -y curl unzip ca-certificates
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL "https://github.com/oven-sh/bun/releases/download/bun-v${BUN_VERSION}/bun-linux-x64.zip" -o "$tmp/bun.zip"
unzip -o "$tmp/bun.zip" -d "$tmp"
install -m 0755 "$tmp/bun-linux-x64/bun" /usr/local/bin/bun
/usr/local/bin/bun --version
