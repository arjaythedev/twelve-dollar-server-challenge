#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Compile with maximum native optimizations and locked dependencies
RUSTFLAGS="-C target-cpu=native" cargo build --release --locked
