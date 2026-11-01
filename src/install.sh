#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl
archive="$(mktemp)"
trap 'rm -f "$archive"' EXIT
curl -fsSL --retry 3 -o "$archive" \
https://github.com/astral-sh/uv/releases/download/0.13.6/uv-x86_64-unknown-linux-gnu.tar.gz
printf '%s  %s\n' f29c3e94ffabd7e6f3ba521dba169a9b7ec12100bf3257087f0f24f6123f3645 "$archive" | sha256sum -c -
tar -xzf "$archive" -C /usr/local/bin --strip-components=1 uv-x86_64-unknown-linux-gnu/uv
