#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p deps
while read -r digest name url; do
  if [[ ! -f "deps/$name" ]] || ! echo "$digest  deps/$name" | sha256sum -c --status; then
    curl -fsSL --retry 3 "$url" -o "deps/$name.part"
    echo "$digest  deps/$name.part" | sha256sum -c -
    mv "deps/$name.part" "deps/$name"
  fi
  echo "$digest  deps/$name" | sha256sum -c -
done < deps.lock
if [[ ! -f deps/sqlite-amalgamation-3540100/sqlite3.c ]]; then
  unzip -q -o deps/sqlite.zip -d deps
fi
if [[ ! -f deps/liburing-2.5/src/include/liburing.h ]]; then
  mkdir -p deps/liburing-2.5
  tar -xzf deps/liburing-2.5.tar.gz --strip-components=1 -C deps/liburing-2.5
fi
