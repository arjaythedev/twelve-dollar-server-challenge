#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs a pinned Bun from the official release zip.
set -euo pipefail
BUN_VERSION=1.4.3
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl unzip
case "$(uname -m)" in aarch64|arm64) ARCH=aarch64 ;; *) ARCH=x64 ;; esac  # arm64 only for local Docker on Apple Silicon
tmp="$(mktemp -d)"
curl -fsSL "https://github.com/oven-sh/bun/releases/download/bun-v${BUN_VERSION}/bun-linux-${ARCH}.zip" -o "$tmp/bun.zip"
unzip -q "$tmp/bun.zip" -d "$tmp"
install -m 0755 "$tmp/bun-linux-${ARCH}/bun" /usr/local/bin/bun
rm -rf "$tmp"
/usr/local/bin/bun --version
