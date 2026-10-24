#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: build tools from apt, plus clang/lld 23.1.4
# from the official LLVM release tarball (pinned by sha256). Only the compiler, linker,
# llvm-profdata and clang's runtime/headers are unpacked. libobjc2, liburing and SQLite
# are built from pinned source tarballs by build.sh.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
LLVM_VER=23.1.4
LLVM_MAJOR=${LLVM_VER%%.*}
LLVM_SHA=efdd1dbd089b99bfcfcde7b8eb1236163f2e88df5a981cca4f783ec9cb5e0378
LLVM_NAME=LLVM-$LLVM_VER-Linux-X64
LLVM_DIR=/opt/llvm-$LLVM_VER

apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  ca-certificates curl build-essential cmake ninja-build unzip zstd

if [ ! -x "$LLVM_DIR/bin/clang-$LLVM_MAJOR" ]; then
  tmp=$(mktemp -d)
  curl -fsSL "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/$LLVM_NAME.tar.zst" -o "$tmp/llvm.tar.zst"
  echo "$LLVM_SHA  $tmp/llvm.tar.zst" | sha256sum -c -
  mkdir -p "$LLVM_DIR"
  # The release is compressed with a 1 GiB window, hence --long.
  zstd -dc --long=31 "$tmp/llvm.tar.zst" | tar -x -C "$LLVM_DIR" --strip-components=1 \
    "$LLVM_NAME/bin/clang-$LLVM_MAJOR" "$LLVM_NAME/bin/clang" "$LLVM_NAME/bin/clang++" \
    "$LLVM_NAME/bin/lld" "$LLVM_NAME/bin/ld.lld" "$LLVM_NAME/bin/llvm-profdata" \
    "$LLVM_NAME/lib/clang/$LLVM_MAJOR/include" \
    "$LLVM_NAME/lib/clang/$LLVM_MAJOR/lib/x86_64-unknown-linux-gnu"
  rm -rf "$tmp"
fi
# build.sh calls clang-23, clang++-23, ld.lld-23 (-fuse-ld=lld-23) and llvm-profdata-23.
ln -sf "$LLVM_DIR/bin/clang-$LLVM_MAJOR"   /usr/local/bin/clang-$LLVM_MAJOR
ln -sf "$LLVM_DIR/bin/clang++"             /usr/local/bin/clang++-$LLVM_MAJOR
ln -sf "$LLVM_DIR/bin/lld"                 /usr/local/bin/ld.lld-$LLVM_MAJOR
ln -sf "$LLVM_DIR/bin/llvm-profdata"       /usr/local/bin/llvm-profdata-$LLVM_MAJOR
clang-$LLVM_MAJOR --version | head -1
