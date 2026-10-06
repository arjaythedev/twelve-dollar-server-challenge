#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source ./sqlite-build.sh
cc -O2 -I"$sqlite" -c tests/commit_fault.c -o .deps/commit_fault.o
export PATH="/opt/twelve-rust/cargo/bin:$HOME/.cargo/bin:$PATH"
if [[ -d /opt/twelve-rust/rustup ]]; then export RUSTUP_HOME=/opt/twelve-rust/rustup; fi
cargo rustc --release --locked -- -C "link-arg=$PWD/.deps/commit_fault.o" -C link-arg=-Wl,--wrap=sqlite3_open_v2
cp target/release/twelve-rust-axum bin/commit-test
python3 tests/commit_check.py
