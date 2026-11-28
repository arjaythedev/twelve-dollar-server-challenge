#!/usr/bin/env bash
# Builds the server as a normal user. Dependencies are pinned:
# - crates via Cargo.lock (committed)
# - uring-kit via git rev a811d22a1d4500d065e5e7d26402b430690165b7
# - SQLite amalgamation via rusqlite "bundled" (cargo fetches + builds it;
#   the build is reproducible from the lockfile)
set -euo pipefail
cd "$(dirname "$0")"

export PATH="/usr/local/bin:$HOME/.cargo/bin:$PATH"

# -O3 via the release profile; target-cpu=native (built on the box, rule 10)
export RUSTFLAGS="-C target-cpu=native"
cargo build --release --locked -p feed-server

test -x target/release/feed-server
echo "build ok"
