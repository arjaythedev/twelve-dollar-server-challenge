#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Installs a pinned Rust toolchain under /usr/local so
# build.sh can use it as a normal user.
set -euo pipefail

apt-get update
apt-get install -y build-essential curl

export RUSTUP_HOME=/usr/local/rustup
export CARGO_HOME=/usr/local/cargo
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs |
	sh -s -- -y --no-modify-path --profile minimal --default-toolchain 1.99.1
chmod -R a+rX "$RUSTUP_HOME" "$CARGO_HOME"
