#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl unzip xz-utils
case $(uname -m) in
  x86_64) arch=x86_64; sha=1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026 ;;
  aarch64) arch=aarch64; sha=9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8 ;;
  *) echo 'Unsupported architecture' >&2; exit 1 ;;
esac
archive=$(mktemp); trap 'rm -f "$archive"' EXIT
curl -fsSL --retry 3 "https://ziglang.org/download/0.17.0/zig-$arch-linux-0.17.0.tar.xz" -o "$archive"
printf '%s  %s\n' "$sha" "$archive" | sha256sum -c -
mkdir -p /opt/zig-0.17.0
tar -xJf "$archive" -C /opt/zig-0.17.0 --strip-components=1
ln -sf /opt/zig-0.17.0/zig /usr/local/bin/zig
