#!/usr/bin/env bash
set -euo pipefail
apt-get update
apt-get install -y build-essential curl git pkg-config libssl-dev cmake clang
# A fixed compiler, installed independently of the root user's home directory.
export RUSTUP_HOME=/opt/baml-rust/rustup CARGO_HOME=/opt/baml-rust/cargo
curl --proto '=https' --tlsv1.2 -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.98.0 --no-modify-path
chmod -R a+rX /opt/baml-rust
