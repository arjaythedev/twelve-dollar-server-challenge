#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

export XMAKE_ROOT=y
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential libssl-dev liburing-dev ca-certificates wget unzip curl

if ! command -v xmake >/dev/null 2>&1; then
  apt-get install -y --no-install-recommends software-properties-common gnupg
  if add-apt-repository -y ppa:xmake-io/xmake && apt-get update; then
    apt-get install -y --no-install-recommends xmake
  else
    curl -fsSL https://xmake.io/shget.text | bash
    ln -sf "$HOME/.local/bin/xmake" /usr/local/bin/xmake
  fi
fi

PH=https://raw.githubusercontent.com/h2o/picohttpparser/54413dc68acd3aca70317d3c0a8c1d45819c9de2
wget -q -O picohttpparser.h "$PH/picohttpparser.h"
wget -q -O picohttpparser.c "$PH/picohttpparser.c"
[ -s picohttpparser.h ] && [ -s picohttpparser.c ] || { echo "picohttpparser download failed" >&2; exit 1; }

SQLITE_VER=3540100
SQLITE_AMAL="3.54.1"
if [ ! -f sqlite3.c ] || [ ! -f sqlite3.h ]; then
  for year in 2026 2025 2024; do
    url="https://www.sqlite.org/$year/sqlite-amalgamation-$SQLITE_VER.zip"
    if wget -q --spider "$url"; then
      wget -q -O /tmp/sq.zip "$url" || continue
      unzip -oq /tmp/sq.zip -d /tmp/sq
      cp "/tmp/sq/sqlite-amalgamation-$SQLITE_VER/sqlite3.c" .
      cp "/tmp/sq/sqlite-amalgamation-$SQLITE_VER/sqlite3.h" .
      break
    fi
  done
fi

[ -f sqlite3.c ] && [ -f sqlite3.h ] || { echo "sqlite download failed" >&2; exit 1; }
grep -q "#define SQLITE_VERSION \"$SQLITE_AMAL\"" sqlite3.h ||
  grep -q "\"$SQLITE_AMAL\"" sqlite3.h ||
  { echo "sqlite version mismatch (want $SQLITE_AMAL)" >&2; exit 1; }

echo "deps ok:"
ls -lh sqlite3.c sqlite3.h picohttpparser.c picohttpparser.h