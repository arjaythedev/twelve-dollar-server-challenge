#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
source ./sqlite-build.sh
export PATH="/opt/twelve-rust/cargo/bin:$HOME/.cargo/bin:$PATH"
if [[ -d /opt/twelve-rust/rustup ]]; then export RUSTUP_HOME=/opt/twelve-rust/rustup; fi
cargo build --release --locked
cp target/release/twelve-rust-axum bin/server
