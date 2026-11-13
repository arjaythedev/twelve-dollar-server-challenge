#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04 box: install Python 3.12, venv tooling, and build toolchain.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  python3 \
  python3-venv \
  python3-pip \
  build-essential \
  curl

# Ensure Rust/Cargo is available if building from source
if ! command -v cargo >/dev/null 2>&1; then
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal
fi
