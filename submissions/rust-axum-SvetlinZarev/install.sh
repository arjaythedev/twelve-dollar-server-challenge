#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates
curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.99.0
. "$HOME/.cargo/env"
rustc --version
