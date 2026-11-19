#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

# Direct HTTP: use the inherited hard descriptor limit (the benchmark sets it to 65535).
hard="$(ulimit -Hn)"
if [ "$hard" != "unlimited" ] && [ "$hard" -gt "$(ulimit -Sn)" ]; then
    ulimit -n "$hard"
fi

exec ./target/release/rust_server
