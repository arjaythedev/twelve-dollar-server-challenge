#!/usr/bin/env bash
# Builds the server as a normal user: fetches the pinned crates (Cargo.lock), including xitdb at
# its pinned git revision, then compiles everything for this CPU.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/bin:$HOME/.cargo/bin:$PATH"
export CARGO_NET_GIT_FETCH_WITH_CLI=true # xitdb comes from git: use the git install.sh installed
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --locked
