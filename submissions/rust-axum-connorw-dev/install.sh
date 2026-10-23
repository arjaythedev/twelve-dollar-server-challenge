#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler (for the bundled SQLite) and a pinned,
# checksum-verified Rust toolchain from the official standalone installer (no rustup).
set -euo pipefail
RUST_VERSION=1.97.0
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential ca-certificates curl xz-utils
[ -x "/opt/rust-$RUST_VERSION/bin/cargo" ] && exit 0
case "$(uname -m)" in
  x86_64) target=x86_64-unknown-linux-gnu ;;
  aarch64) target=aarch64-unknown-linux-gnu ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
archive="rust-$RUST_VERSION-$target.tar.xz"
curl -fsSL --retry 3 -o "$tmp/$archive" "https://static.rust-lang.org/dist/$archive"
curl -fsSL --retry 3 -o "$tmp/$archive.sha256" "https://static.rust-lang.org/dist/$archive.sha256"
(cd "$tmp" && sha256sum --check --quiet "$archive.sha256")
tar -xJf "$tmp/$archive" -C "$tmp"
"$tmp/rust-$RUST_VERSION-$target/install.sh" --prefix="/opt/rust-$RUST_VERSION" --without=rust-docs >/dev/null
chmod -R a+rX "/opt/rust-$RUST_VERSION"
