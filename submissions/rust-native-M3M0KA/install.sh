#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Installs the C toolchain, archive
# tools, and a checksum-verified Rust 1.94.0 compiler under /opt, readable and
# executable by any user.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential ca-certificates curl tar xz-utils unzip >/dev/null

RUST_VERSION=1.94.0
RUST_TRIPLE=x86_64-unknown-linux-gnu
RUST_ARCHIVE="rust-${RUST_VERSION}-${RUST_TRIPLE}.tar.xz"
RUST_URL="https://static.rust-lang.org/dist/${RUST_ARCHIVE}"
RUST_SHA256="e8fa4185f3ef6ae32725ff638b1ecdbff28f5d651dc0b3111e2539350d03b15a"
RUST_PREFIX=/opt/twelve-rust

if [ ! -x "$RUST_PREFIX/bin/rustc" ] || ! "$RUST_PREFIX/bin/rustc" --version | grep -Fq "rustc ${RUST_VERSION} "; then
  rm -rf /tmp/rust-install "$RUST_PREFIX"
  mkdir -p /tmp/rust-install
  curl -fsSL --retry 3 -o "/tmp/rust-install/${RUST_ARCHIVE}" "$RUST_URL"
  echo "${RUST_SHA256}  /tmp/rust-install/${RUST_ARCHIVE}" | sha256sum -c -
  tar -xJf "/tmp/rust-install/${RUST_ARCHIVE}" --strip-components=1 -C /tmp/rust-install
  /tmp/rust-install/install.sh \
    --prefix="$RUST_PREFIX" \
    --components="rustc,rust-std-${RUST_TRIPLE}" \
    --disable-ldconfig >/dev/null
  rm -rf /tmp/rust-install
fi

chmod -R a+rX "$RUST_PREFIX"
ln -sf "$RUST_PREFIX/bin/rustc" /usr/local/bin/rustc
rustc --version
