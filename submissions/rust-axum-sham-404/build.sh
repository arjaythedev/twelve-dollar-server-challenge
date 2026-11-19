#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if [ -f "$HOME/.cargo/env" ]; then
    # shellcheck disable=SC1091
    source "$HOME/.cargo/env"
fi

# The benchmark box builds and runs on the same machine, so tune for its CPU.
export RUSTFLAGS="${RUSTFLAGS:+$RUSTFLAGS }-C target-cpu=native"

cargo build --release
