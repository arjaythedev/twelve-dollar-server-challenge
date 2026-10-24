#!/usr/bin/env bash
# Runs the official bench/load.js with a pinned k6 release downloaded into
# tools/k6/ (no root needed). k6 needs roughly 0.3 MB per VU; keep local runs
# to a few thousand VUs.
#
#   tools/k6.sh [VUS] [BASE_URL] [extra k6 args...]
set -euo pipefail
cd "$(dirname "$0")/.."
K6_VER=v2.3.0
K6_SHA256=39c3117b6af817592dcd0ce4242105c0a7af10948c2a425306f0be8f7a8a8ab1
K6_DIR=tools/k6
VUS="${1:-1000}"
BASE_URL="${2:-http://127.0.0.1:3000}"
shift $(( $# > 2 ? 2 : $# ))
if [ ! -x "$K6_DIR/k6" ]; then
  mkdir -p "$K6_DIR"
  curl -fsSL -o "$K6_DIR/k6.tar.gz" "https://github.com/grafana/k6/releases/download/$K6_VER/k6-$K6_VER-linux-amd64.tar.gz"
  echo "$K6_SHA256  $K6_DIR/k6.tar.gz" | sha256sum -c --quiet
  tar -xzf "$K6_DIR/k6.tar.gz" -C "$K6_DIR" --strip-components=1 "k6-$K6_VER-linux-amd64/k6"
  rm -f "$K6_DIR/k6.tar.gz"
fi
ROOT="$(cd ../.. && pwd)"
exec "$K6_DIR/k6" run -e VUS="$VUS" -e BASE_URL="$BASE_URL" -e TOKENS="$ROOT/seed/tokens.json" "$@" "$ROOT/bench/load.js"
