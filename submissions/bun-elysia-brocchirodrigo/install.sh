#!/usr/bin/env bash
set -euo pipefail
BUN_VERSION=1.4.2

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl unzip

case "$(uname -m)" in
  x86_64)  target=x64; grep -qw avx2 /proc/cpuinfo || target=x64-baseline ;;
  aarch64) target=aarch64 ;;
  *) echo "unsupported architecture: $(uname -m)"; exit 1 ;;
esac

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
base="https://github.com/oven-sh/bun/releases/download/bun-v$BUN_VERSION"
curl -fsSL -o "$tmp/bun-linux-$target.zip" "$base/bun-linux-$target.zip"
curl -fsSL -o "$tmp/SHASUMS256.txt" "$base/SHASUMS256.txt"
(cd "$tmp" && grep " bun-linux-$target.zip\$" SHASUMS256.txt | sha256sum -c -)
unzip -q "$tmp/bun-linux-$target.zip" -d "$tmp"
install -m 0755 "$tmp/bun-linux-$target/bun" /usr/local/bin/bun
bun --version
