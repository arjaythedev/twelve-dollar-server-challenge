#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler and git to build the Saad compiler, LLVM 18 for its optimized
# backend (--llvm), and SQLite. The versions are Ubuntu 24.04's
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends build-essential git ca-certificates llvm-18-dev libzstd-dev zlib1g-dev libsqlite3-dev
