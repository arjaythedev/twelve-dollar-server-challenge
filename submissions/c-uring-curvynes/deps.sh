#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p .deps
fetch() {
  local url="$1" destination="$2" expected="$3"
  if [[ ! -f "$destination" ]]; then
    curl --fail --location --retry 3 "$url" -o "$destination.tmp"
    mv "$destination.tmp" "$destination"
  fi
  printf '%s  %s\n' "$expected" "$destination" | sha256sum --check --status
}
# Downloaded sources are pinned by release and SHA-256 in deps.lock.
while read -r digest name url; do
  [[ -z "$digest" || "$digest" == \#* ]] && continue
  fetch "$url" ".deps/$name" "$digest"
done < deps.lock
if [[ ! -f .deps/sqlite/source-version || "$(cat .deps/sqlite/source-version)" != 3.54.1 ]]; then
  unzip -q -o .deps/sqlite-3540100.zip -d .deps
  mkdir -p .deps/sqlite
  cp .deps/sqlite-amalgamation-3540100/{sqlite3.c,sqlite3.h,sqlite3ext.h} .deps/sqlite/
  printf '%s\n' 3.54.1 > .deps/sqlite/source-version
fi
if [[ ! -f .deps/liburing/src/liburing.a ]]; then
  tar -xzf .deps/liburing.tar.gz -C .deps
  (cd .deps/liburing-liburing-2.6 && ./configure --cc="${CC:-cc}" && make -j1 -C src)
  mkdir -p .deps/liburing
  cp -a .deps/liburing-liburing-2.6/src .deps/liburing/
fi
