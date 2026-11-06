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
go build -o bin/server .
