#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/bin:$HOME/.cargo/bin:$PATH"
RUSTFLAGS="-C target-cpu=native" cargo build --release --locked
