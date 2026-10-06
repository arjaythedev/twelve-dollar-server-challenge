#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl unzip pkg-config
export CARGO_HOME=/opt/twelve-rust/cargo RUSTUP_HOME=/opt/twelve-rust/rustup
installer=$(mktemp); trap 'rm -f "$installer"' EXIT
curl -fsSL --retry 3 https://sh.rustup.rs -o "$installer"
sh "$installer" -y --no-modify-path --profile minimal --default-toolchain 1.93.0
chmod -R a+rX /opt/twelve-rust
