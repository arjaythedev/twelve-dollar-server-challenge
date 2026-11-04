#!/usr/bin/env bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip xz-utils

RUST_VERSION="1.94.0"
export RUSTUP_HOME="/opt/tvcc-rust/rustup"
export CARGO_HOME="/opt/tvcc-rust/cargo"

if [ ! -x "/opt/tvcc-rust/cargo/bin/cargo" ]; then
  curl -fsSL https://sh.rustup.rs -o /tmp/tvcc-rustup.sh
  sh /tmp/tvcc-rustup.sh -y --profile minimal --default-toolchain "${RUST_VERSION}" --no-modify-path
  rm -f /tmp/tvcc-rustup.sh
  chmod -R a+rX /opt/tvcc-rust
  for tool in cargo rustc; do
    printf '#!/bin/sh\nexport RUSTUP_HOME=/opt/tvcc-rust/rustup\nexec /opt/tvcc-rust/cargo/bin/%s "$@"\n' "$tool" > "/usr/local/bin/$tool"
    chmod 755 "/usr/local/bin/$tool"
  done
fi

