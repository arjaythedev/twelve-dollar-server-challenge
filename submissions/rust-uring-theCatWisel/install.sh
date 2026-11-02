#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip xz-utils

if command -v cargo >/dev/null 2>&1 && command -v rustc >/dev/null 2>&1; then
  exit 0
fi

ARCH="$(uname -m)"
case "$ARCH" in
  x86_64) SHA=e8fa4185f3ef6ae32725ff638b1ecdbff28f5d651dc0b3111e2539350d03b15a ;;
  aarch64) SHA=c6fd6d1c925ed986df3b2c0b89bbc90ce15afb62e4d522a054e7d50c856b3c1a ;;
  *) echo "unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

PKG="rust-1.94.0-$ARCH-unknown-linux-gnu"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fsSL --retry 3 -o "$TMP/$PKG.tar.xz" "https://static.rust-lang.org/dist/$PKG.tar.xz"
echo "$SHA  $TMP/$PKG.tar.xz" | sha256sum -c -
tar -xJf "$TMP/$PKG.tar.xz" -C "$TMP"
"$TMP/$PKG/install.sh" --prefix=/usr/local --components=rustc,cargo,"rust-std-$ARCH-unknown-linux-gnu"
chmod -R a+rX /usr/local/lib/rustlib /usr/local/bin/cargo /usr/local/bin/rustc
