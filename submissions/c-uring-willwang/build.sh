#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
sqlite=sqlite-amalgamation-3540100
digest=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
sources_ok() {
  echo '0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52  vendor/sqlite3.c
d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c  vendor/sqlite3.h' | sha256sum --check --status
}
if ! sources_ok 2>/dev/null; then
  mkdir -p vendor
  stage=$(mktemp -d vendor/fetch.XXXXXXXX)
  trap 'rm -rf "$stage"' EXIT
  curl -fsSL --retry 3 "https://www.sqlite.org/2026/$sqlite.zip" -o "$stage/source.zip"
  echo "$digest  $stage/source.zip" | sha256sum --check --status
  unzip -jq "$stage/source.zip" "$sqlite/sqlite3.c" "$sqlite/sqlite3.h" -d "$stage"
  mv "$stage/sqlite3.c" "$stage/sqlite3.h" vendor/
  sources_ok
fi
make -j4
