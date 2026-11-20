#!/usr/bin/env bash
# Builds bin/server as a normal user: fetches the pinned SQLite amalgamation (sha256-checked)
# into sqlite/, then compiles SQLite + the shim (C) and the server (Go) for this machine.
# No Go modules are downloaded; the only dependency is the Go standard library.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/go/bin:$PATH"
SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
if [ ! -f sqlite/sqlite3.c ]; then
  mkdir -p sqlite
  curl -fsSL --retry 3 -o "sqlite/$SQ.zip" "https://www.sqlite.org/2026/$SQ.zip"
  echo "$SQ_SHA  sqlite/$SQ.zip" | sha256sum -c -
  (cd sqlite && unzip -qo "$SQ.zip" && mv "$SQ/sqlite3.c" "$SQ/sqlite3.h" . && rm -rf "$SQ" "$SQ.zip")
fi
export CGO_ENABLED=1 GOFLAGS=-trimpath GOTOOLCHAIN=local

# C side (SQLite + shim, one translation unit): tuned for the build CPU. Ubuntu's gcc adds stack
# protectors, CET and fortify checks by default; none of them helps a single-threaded server.
CFLAGS="-O3 -march=native -fno-plt -fno-semantic-interposition -fno-stack-protector"
CFLAGS="$CFLAGS -fno-stack-clash-protection -U_FORTIFY_SOURCE -fomit-frame-pointer -w"
GOAMD64=v1
if [ "$(uname -m)" = x86_64 ]; then
  CFLAGS="$CFLAGS -fcf-protection=none -mprefer-vector-width=256"
  has() { for f in "$@"; do grep -qw "$f" /proc/cpuinfo || return 1; done; }
  # Go's amd64 levels: v3 = AVX2/BMI2/FMA/MOVBE (Haswell+), v4 = v3 + AVX-512 F/BW/DQ/VL.
  if has avx2 bmi1 bmi2 fma movbe f16c abm; then
    GOAMD64=v3
    has avx512f avx512bw avx512dq avx512vl && GOAMD64=v4
  fi
fi
export CGO_CFLAGS="$CFLAGS" GOAMD64
echo "GOAMD64=$GOAMD64  CGO_CFLAGS=$CGO_CFLAGS"
# -B: no bounds checks in Go code (the request path indexes fixed tables with masked indices).
mkdir -p bin
go build -gcflags=all=-B -ldflags=-s -o bin/server .
echo "built bin/server ($(stat -c %s bin/server) bytes)"
