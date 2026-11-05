#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Installs the C toolchain plus a
# pinned Rust toolchain under /opt, readable and executable by any user: the
# challenge builds as the unprivileged `deploy` user, not as root.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential ca-certificates curl >/dev/null

RUST_VERSION=1.94.0
RUSTUP_HOME=/opt/twelve-rust/rustup
export RUSTUP_HOME
export CARGO_HOME=/opt/twelve-rust/cargo
if [ ! -x "$CARGO_HOME/bin/rustc" ]; then
  curl -fsSL https://sh.rustup.rs -o /tmp/rustup.sh
  sh /tmp/rustup.sh -y --profile minimal --default-toolchain "$RUST_VERSION" --no-modify-path
  rm -f /tmp/rustup.sh
fi
chmod -R a+rX /opt/twelve-rust
for tool in rustc cargo; do
  printf '#!/bin/sh\nexport RUSTUP_HOME=%s\nexec %s/bin/%s "$@"\n' \
    "$RUSTUP_HOME" "$CARGO_HOME" "$tool" > "/usr/local/bin/$tool"
  chmod 755 "/usr/local/bin/$tool"
done
rustc --version
