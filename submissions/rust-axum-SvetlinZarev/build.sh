#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
cargo build --release --locked
