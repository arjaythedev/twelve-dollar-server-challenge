#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the C toolchain, python3 (build.sh's profile
# training), and the Goose compiler, built from its pinned source (sha256-checked) into /opt and
# linked onto the build user's PATH.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
if [[ $(id -u) == 0 ]]; then
  apt-get update
  apt-get install -y --no-install-recommends build-essential cmake ninja-build curl ca-certificates unzip python3
fi

GOOSE_COMMIT=369c7a2cfca74bfcee60379727e8bdfd83da2d75
GOOSE_SHA256=83dd689acb314930bb8f38a911d9bb9fec00b8dad0d3191eabe1c837d2f33f12
prefix=${GOOSE_PREFIX:-/opt/goose-${GOOSE_COMMIT:0:8}}

if [[ ! -x $prefix/build/goose ]]; then
  archive=$(mktemp)
  trap 'rm -f "$archive"' EXIT
  curl -fsSL --retry 3 -o "$archive" "https://codeload.github.com/aardappel/goose/tar.gz/$GOOSE_COMMIT"
  echo "$GOOSE_SHA256  $archive" | sha256sum -c -
  rm -rf "$prefix"
  mkdir -p "$prefix"
  tar -xzf "$archive" --strip-components=1 -C "$prefix"
  # The compiler is one large C++20 translation unit, and at gcc's defaults compiling it needs more
  # memory than the 2 GB box has. One job, -O2 and a garbage collector that runs more often keep it
  # within what the box has (the settings goose-native-tbsvttr #143 arrived at on the box). How the
  # compiler itself is optimized does not change the C it writes; build.sh compiles that at -O3.
  cmake -S "$prefix" -B "$prefix/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGOOSE_BUILTIN_LIBS= \
    "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG --param ggc-min-expand=5 --param ggc-min-heapsize=16384" \
    "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG"
  cmake --build "$prefix/build" --parallel 1
fi
if [[ $(id -u) == 0 ]]; then ln -sf "$prefix/build/goose" /usr/local/bin/goose; fi
"$prefix/build/goose" version
