#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: C toolchain + pinned Rust.
set -euo pipefail

apt-get update
apt-get install -y build-essential curl ca-certificates xz-utils

ARCH="$(uname -m)"
# x86_64 only: the benchmark box is a DigitalOcean Basic droplet (x86_64).
# SHA from the official dist manifest for rust-1.94.0.
case "$ARCH" in
  x86_64) SHA=e8fa4185f3ef6ae32725ff638b1ecdbff28f5d651dc0b3111e2539350d03b15a ;;
  *) echo "unsupported architecture $ARCH (x86_64 only)"; exit 1 ;;
esac
PKG="rust-1.94.0-$ARCH-unknown-linux-gnu"
TMP="$(mktemp -d)"

if command -v rustc >/dev/null 2>&1 && rustc --version | grep -q '1.94.0'; then
  echo "rust 1.94.0 already present"
else
  curl -fsSL --retry 3 -o "$TMP/rust.tar.xz" \
    "https://static.rust-lang.org/dist/$PKG.tar.xz"
  echo "$SHA  $TMP/rust.tar.xz" | sha256sum -c -
  # use the tarball's own installer: it wires the component layout
  # (rustc, cargo, rust-std) into the prefix correctly
  tar -xJf "$TMP/rust.tar.xz" -C "$TMP"
  "$TMP/$PKG/install.sh" --prefix=/usr/local
  rm -rf "$TMP"
fi

rustc --version
cargo --version
