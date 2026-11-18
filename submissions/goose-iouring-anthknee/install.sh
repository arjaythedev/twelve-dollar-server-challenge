#!/usr/bin/env bash
# Ubuntu 24.04 toolchain installation. Application dependencies stay under .work.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
if [[ $(id -u) != 0 ]]; then
  echo 'Run install.sh as root on Ubuntu 24.04.' >&2
  exit 1
fi
apt=(apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30)
"${apt[@]}" update
"${apt[@]}" install -y --no-install-recommends build-essential cmake ninja-build curl ca-certificates unzip python3 git

version=2026.4
sha=943d785fb191de7b52539e9cf16715701babcacd339323f786729c2132343d9b
# A new prefix keeps earlier assertion-disabled builds from being reused.
prefix=/opt/goose-anthknee-2026.4-r1
if [[ -e $prefix ]]; then
  if [[ ! -x $prefix/build/goose ]] || [[ $("$prefix/build/goose" version) != "goose v$version "* ]]; then
    echo "Existing $prefix is not the expected compiler; inspect it before replacing it." >&2
    exit 1
  fi
else
  stage=$(mktemp -d /opt/goose-anthknee-build.XXXXXX)
  trap 'rm -rf "$stage"' EXIT
  curl -fsSL --connect-timeout 30 --max-time 300 --retry 3 "https://codeload.github.com/aardappel/goose/tar.gz/refs/tags/v$version" -o "$stage/source.tar.gz"
  printf '%s  %s\n' "$sha" "$stage/source.tar.gz" | sha256sum -c -
  mkdir "$stage/source"
  tar -xzf "$stage/source.tar.gz" --strip-components=1 -C "$stage/source"
  # The hosted JIT exits through longjmp. Its forward declaration must say so.
  # Shared runtime headers also define helpers unused by this compiler object.
  python3 - "$stage/source" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
p = root / "src/jit_runtime.c"
s = p.read_text()
before = "static void gs_jit_exit(int code);"
if s.count(before) != 1:
    raise SystemExit("Unexpected Goose 2026.4 JIT declaration")
p.write_text(s.replace(before, "static _Noreturn void gs_jit_exit(int code);"))
with (root / "CMakeLists.txt").open("a") as f:
    f.write('\nset_source_files_properties(src/jit_runtime.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")\n')
PY
  # One job and frequent GCC collection keep the compiler build within the 2 GiB guest.
  cmake -S "$stage/source" -B "$stage/source/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DGOOSE_VERSION=v2026.4 \
    '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -UNDEBUG -Werror --param ggc-min-expand=5 --param ggc-min-heapsize=16384' \
    '-DCMAKE_C_FLAGS_RELEASE=-O2 -UNDEBUG -Werror'
  cmake --build "$stage/source/build" --parallel 1
  mv "$stage/source" "$prefix"
fi
"$prefix/build/goose" version
