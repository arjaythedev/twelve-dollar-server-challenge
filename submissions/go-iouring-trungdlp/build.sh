#!/usr/bin/env bash
# Builds as a normal user: fetches the pinned SQLite amalgamation (sha256-checked) into sqlite/,
# then compiles the server and SQLite for this CPU. No Go modules are downloaded.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/go/bin:$PATH"
SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
if [ ! -f sqlite/sqlite3.c ]; then
  mkdir -p sqlite
  curl -fsSL --retry 3 -o sqlite/$SQ.zip "https://www.sqlite.org/2026/$SQ.zip"
  echo "$SQ_SHA  sqlite/$SQ.zip" | sha256sum -c -
  (cd sqlite && unzip -qo $SQ.zip && mv $SQ/sqlite3.c $SQ/sqlite3.h . && rm -rf $SQ $SQ.zip)
fi
export CGO_ENABLED=1 GOFLAGS=-trimpath GOTOOLCHAIN=local
# C: no stack protector, CET or fortify checks (Ubuntu's gcc adds them by default). Go: generate
# code for x86-64-v3 (AVX2, BMI2, ...) when the CPU has it; Go's default is the 2003 baseline.
export CGO_CFLAGS="-O2 -fno-stack-protector -U_FORTIFY_SOURCE"
if [ "$(uname -m)" = x86_64 ]; then
  CGO_CFLAGS="$CGO_CFLAGS -fcf-protection=none"
  v3=1
  for f in avx2 bmi1 bmi2 fma movbe f16c lzcnt; do
    [ "$f" = lzcnt ] && f=abm
    grep -qw "$f" /proc/cpuinfo || v3=0
  done
  [ $v3 = 1 ] && export GOAMD64=v3
fi
go build -o bin/server .
