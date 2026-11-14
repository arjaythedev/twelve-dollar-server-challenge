#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. gcc and binutils come from apt (build-essential: gcc builds the
# stock sqlite3 shell, `as` and `ld` build src/serve.s). Two release tarballs, pinned by version and sha256,
# go to /opt/wat-sqx:
#   wabt 1.0.39        wat2wasm: src/app.wat -> wasm
#   WasmEdge 0.18.1    `wasmedge compile` (AOT, LLVM) and libwasmedge, the runtime serve.so loads app.so with
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
T=/opt/wat-sqx
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
[ -x $T/wasmedge/bin/wasmedge ] || get https://github.com/WasmEdge/WasmEdge/releases/download/0.18.1/WasmEdge-0.18.1-ubuntu20.04_x86_64.tar.gz \
  c55574ab932298b0c15ebe108f401a324a2b06e1e30ba426253db2d921893603 $T/wasmedge 0
$T/wabt/bin/wat2wasm --version; LD_LIBRARY_PATH=$T/wasmedge/lib $T/wasmedge/bin/wasmedge --version
