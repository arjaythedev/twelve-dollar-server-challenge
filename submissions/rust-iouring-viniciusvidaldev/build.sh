#!/usr/bin/env bash
# Runs as a normal user. Uses the toolchain from install.sh; crates download into ~/.cargo.
set -euo pipefail
cd "$(dirname "$0")"

export RUSTUP_HOME=/usr/local/rustup
/usr/local/cargo/bin/cargo build --release --locked
