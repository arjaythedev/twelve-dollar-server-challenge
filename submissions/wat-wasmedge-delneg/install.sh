#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Installs the toolchain from official release tarballs,
# each pinned by version and sha256, into /opt/wat-tdsc:
#   wabt 1.0.39        wat2wasm: the hand-written WAT -> wasm
#   wasi-sdk 34        clang for wasm32-wasip1: compiles stock SQLite (#included by sqlite_glue.c, which adds the exports) to wasm
#   binaryen 125       wasm-merge: joins the WAT module and the SQLite module into one; wasm-opt -O3 on the result
#   WasmEdge 0.18.1    the runtime: `wasmedge compile` (AOT, LLVM) and libwasmedge for the host in src/host.c
# The app itself (and SQLite, pinned in build.sh) is built from source by build.sh.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
T=/opt/wat-tdsc
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates curl build-essential unzip zlib1g libtinfo6 >/dev/null

get() { # url sha256 dir [strip]
  local f; f=$(mktemp)
  curl -fsSL "$1" -o "$f"
  echo "$2  $f" | sha256sum -c - >/dev/null
  rm -rf "$3"; mkdir -p "$3"; tar xzf "$f" -C "$3" --strip-components="${4:-1}"; rm -f "$f"
}
mkdir -p $T
[ -x $T/wabt/bin/wat2wasm ] || get https://github.com/WebAssembly/wabt/releases/download/1.0.39/wabt-1.0.39-linux-x64.tar.gz \
  1df1254b6639f5f1f89665907ee8cd0cd30114f40483515e583cccb86240840f $T/wabt
[ -x $T/wasi-sdk/bin/clang ] || get https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-34/wasi-sdk-34.0-x86_64-linux.tar.gz \
  b761e3a0721dbae9c09a0059e5fdb2bf917d1b4a8a7b430fb3b5aafb0984b2c4 $T/wasi-sdk
[ -x $T/binaryen/bin/wasm-merge ] || get https://github.com/WebAssembly/binaryen/releases/download/version_125/binaryen-version_125-x86_64-linux.tar.gz \
  7c3bc16599c8274a04d34a504fe4be2047884f900e0e2da2f6fb9cd667183be4 $T/binaryen
[ -x $T/wasmedge/bin/wasmedge ] || get https://github.com/WasmEdge/WasmEdge/releases/download/0.18.1/WasmEdge-0.18.1-ubuntu20.04_x86_64.tar.gz \
  c55574ab932298b0c15ebe108f401a324a2b06e1e30ba426253db2d921893603 $T/wasmedge 0
$T/wabt/bin/wat2wasm --version; $T/wasi-sdk/bin/clang --version | head -1
$T/binaryen/bin/wasm-merge --version; LD_LIBRARY_PATH=$T/wasmedge/lib $T/wasmedge/bin/wasmedge --version
