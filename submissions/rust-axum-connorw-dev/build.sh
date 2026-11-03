#!/usr/bin/env bash
# Builds the server as a normal user. Dependencies are pinned in Cargo.toml and Cargo.lock.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/rust-1.97.0/bin:$PATH"
cargo build --release --locked
