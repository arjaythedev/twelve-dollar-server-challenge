#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/bin:$HOME/.cargo/bin:$PATH"
cargo build --release --locked
