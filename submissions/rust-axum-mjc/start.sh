#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
ulimit -S -n "$(ulimit -H -n)"
exec "$HERE/target/release/twelve-dollar-server"
