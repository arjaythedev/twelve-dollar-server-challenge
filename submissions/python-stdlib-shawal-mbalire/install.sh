#!/usr/bin/env bash
# Required by the challenge. Runs once as root on a clean Ubuntu 24.04: python3 runs infra/ops.py, clang/lld/llvm 19 compile CPython.
set -e; apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates python3 make pkg-config libc6-dev clang-19 lld-19 llvm-19 libclang-rt-19-dev
