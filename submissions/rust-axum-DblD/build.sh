#!/usr/bin/env bash
# Runs as a normal user. Dependencies are pinned in Cargo.toml and Cargo.lock.
set -euo pipefail
cd "$(dirname "$0")"
[ -d /opt/rust-1.99.1/bin ] && export PATH="/opt/rust-1.99.1/bin:$PATH"
cargo build --release --locked
