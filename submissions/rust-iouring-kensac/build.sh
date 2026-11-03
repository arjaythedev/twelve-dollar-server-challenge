#!/usr/bin/env bash
# Builds the server as a normal user: fetches the pinned SQLite amalgamation (sha256-checked) and
# the pinned crates (Cargo.lock), then compiles everything for this CPU.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/bin:$HOME/.cargo/bin:$PATH"
SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p vendor
if [ ! -f vendor/sqlite3.c ]; then
  curl -fsSL --retry 3 -o vendor/$SQ.zip "https://www.sqlite.org/2026/$SQ.zip"
  echo "$SQ_SHA  vendor/$SQ.zip" | sha256sum -c -
  (cd vendor && unzip -qo $SQ.zip && mv $SQ/sqlite3.c $SQ/sqlite3.h . && rm -rf $SQ $SQ.zip)
fi
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --locked
