#!/usr/bin/env bash
# Fetches the two pinned dependencies into third_party/ and verifies them:
#   SQLite 3.54.1 amalgamation (SHA3-256 as published on sqlite.org/download.html)
#   liburing 2.15 (SHA-256 of the GitHub release tarball)
# Builds liburing's static library. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"
TP=third_party
SQLITE_VER=3540100
SQLITE_URL="https://sqlite.org/2026/sqlite-amalgamation-$SQLITE_VER.zip"
SQLITE_SHA3=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
URING_VER=2.15
URING_URL="https://github.com/axboe/liburing/archive/refs/tags/liburing-$URING_VER.tar.gz"
URING_SHA256=8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a

mkdir -p "$TP"
if [ ! -f "$TP/sqlite-amalgamation-$SQLITE_VER/sqlite3.c" ]; then
  echo "deps: fetching SQLite $SQLITE_VER"
  curl -fsSL --retry 3 -o "$TP/sqlite.zip" "$SQLITE_URL"
  got="$(openssl dgst -sha3-256 "$TP/sqlite.zip" | awk '{print $NF}')"
  [ "$got" = "$SQLITE_SHA3" ] || { echo "sqlite: SHA3-256 mismatch: $got" >&2; exit 1; }
  unzip -q -o "$TP/sqlite.zip" -d "$TP"
  rm -f "$TP/sqlite.zip"
fi
if [ ! -f "$TP/liburing-liburing-$URING_VER/src/liburing.a" ]; then
  echo "deps: fetching liburing $URING_VER"
  curl -fsSL --retry 3 -o "$TP/liburing.tar.gz" "$URING_URL"
  echo "$URING_SHA256  $TP/liburing.tar.gz" | sha256sum -c --quiet
  tar -xzf "$TP/liburing.tar.gz" -C "$TP"
  rm -f "$TP/liburing.tar.gz"
  (cd "$TP/liburing-liburing-$URING_VER" && ./configure >/dev/null && make -s -j2 -C src liburing.a >/dev/null)
fi
echo "deps: ready"
