#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C toolchain (cgo, for the SQLite driver) and Go 1.27.1.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl unzip
case $(uname -m) in
  x86_64) arch=amd64; hash=63d339f0da5ab53635a56f2490a7984dfe12dfcff22ad749f63edaf590168445 ;;
  aarch64) arch=arm64; hash=3450b45a3f9ee8568792736a5c5e70a1f2e9b36c35a8f74958c03e51d7d92bec ;;
  *) echo "unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
archive=$(mktemp); trap 'rm -f "$archive"' EXIT
curl -fsSL --retry 3 "https://go.dev/dl/go1.27.1.linux-$arch.tar.gz" -o "$archive"
printf '%s  %s\n' "$hash" "$archive" | sha256sum -c -
rm -rf /usr/local/go && tar -C /usr/local -xzf "$archive"
