#!/usr/bin/env bash
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends build-essential clang-18 llvm-18-linker-tools binutils ca-certificates curl unzip linux-libc-dev 'libssl-dev=3.0.13-*'
