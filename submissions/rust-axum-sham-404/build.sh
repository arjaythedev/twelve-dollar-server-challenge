#!/usr/bin/env bash
# Runs as a normal user. Dependencies are pinned in Cargo.toml and Cargo.lock,
# and --locked fails the build instead of silently re-resolving them.
set -euo pipefail

cd "$(dirname "$0")"

# install.sh puts the pinned toolchain in /opt so any user can build.
export PATH="/opt/rust-1.99.1/bin:$PATH"

# The benchmark box builds and runs on the same machine, so tune for its CPU.
export RUSTFLAGS="${RUSTFLAGS:+$RUSTFLAGS }-C target-cpu=native"

cargo build --release --locked
