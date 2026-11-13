#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] && export XMAKE_ROOT=y

SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
if [ ! -f sqlite3.c ] && [ ! -f vendor/sqlite3.c ]; then
  mkdir -p vendor
  if [ ! -f vendor/$SQ.zip ]; then
    curl -fsSL --retry 3 -o vendor/$SQ.zip "https://www.sqlite.org/2026/$SQ.zip"
  fi
  echo "$SQ_SHA  vendor/$SQ.zip" | sha256sum -c -
  (cd vendor && unzip -qo $SQ.zip && mv $SQ/sqlite3.c $SQ/sqlite3.h . && rm -rf $SQ $SQ.zip)
fi

xmake f -c -m release -y
xmake -j"$(nproc)" -y
[ -x server ] || { echo "build failed: no ./server" >&2; exit 1; }
ls -l server
