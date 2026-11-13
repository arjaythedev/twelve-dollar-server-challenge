#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] && export XMAKE_ROOT=y

xmake f -c -m release -y
xmake -j"$(nproc)" -y
[ -x server ] || { echo "build failed" >&2; exit 1; }
ls -l server