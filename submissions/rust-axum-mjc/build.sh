#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
cd "$HERE"
if [[ -d /opt/rust-stable ]]; then
  export PATH="/opt/rust-stable/bin:/opt/mold-3.0.0/bin:$PATH"
  export MOLD_LIBDIR=/opt/mold-3.0.0/lib/mold
  export RUSTFLAGS="-C force-frame-pointers=yes -C link-arg=-fuse-ld=mold"
fi
exec cargo build --release --locked --jobs 1
