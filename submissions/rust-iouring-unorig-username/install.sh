#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler (for SQLite) and Rust 1.90.0 via rustup-init 1.28.2 (sha256-checked) in /opt/rust.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip
export RUSTUP_HOME=/opt/rust CARGO_HOME=/opt/rust/cargo
ARCH=$(uname -m)
curl -fsSL https://static.rust-lang.org/rustup/archive/1.28.2/$ARCH-unknown-linux-gnu/rustup-init -o /tmp/rustup-init
case $ARCH in
  x86_64)  SHA=20a06e644b0d9bd2fbdbfd52d42540bdde820ea7df86e92e533c073da0cdd43c ;;
  aarch64) SHA=e3853c5a252fca15252d07cb23a1bdd9377a8c6f3efa01531109281ae47f841c ;;
esac
echo "$SHA  /tmp/rustup-init" | sha256sum -c -
chmod +x /tmp/rustup-init
/tmp/rustup-init -y --no-modify-path --profile minimal --default-toolchain 1.90.0
chmod -R a+rX /opt/rust
/opt/rust/cargo/bin/cargo --version
