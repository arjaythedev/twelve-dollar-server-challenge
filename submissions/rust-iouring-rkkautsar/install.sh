#!/usr/bin/env bash
set -euo pipefail
apt-get update
apt-get install -y build-essential curl ca-certificates xz-utils
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64) RUST_ARCH=x86_64-unknown-linux-gnu ;;
  aarch64) RUST_ARCH=aarch64-unknown-linux-gnu ;;
  *) echo "unsupported architecture: $ARCH" >&2; exit 1 ;;
esac
PKG="rust-1.94.0-$RUST_ARCH"
TMP="$(mktemp -d)"
curl -fsSL --retry 3 -o "$TMP/$PKG.tar.xz" "https://static.rust-lang.org/dist/$PKG.tar.xz"
tar -xJf "$TMP/$PKG.tar.xz" -C "$TMP"
"$TMP/$PKG/install.sh" --prefix=/usr/local --components=rustc,cargo,"rust-std-$RUST_ARCH"
rm -rf "$TMP"
cargo --version
