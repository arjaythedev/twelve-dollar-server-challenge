#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: clang 23 from apt.llvm.org plus build tools.
# libobjc2, liburing and SQLite are built from pinned source tarballs by build.sh.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
LLVM_MAJOR=23
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  ca-certificates curl gnupg build-essential cmake ninja-build unzip
install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor --yes -o /etc/apt/keyrings/apt.llvm.org.gpg
echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-$LLVM_MAJOR main" \
  > /etc/apt/sources.list.d/llvm-$LLVM_MAJOR.list
apt-get update -qq
apt-get install -y -qq --no-install-recommends clang-$LLVM_MAJOR lld-$LLVM_MAJOR llvm-$LLVM_MAJOR libclang-rt-$LLVM_MAJOR-dev
clang-$LLVM_MAJOR --version | head -1
