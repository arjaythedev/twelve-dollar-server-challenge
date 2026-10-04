#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p .deps bin
checksum=(shasum -a 256)
if command -v sha256sum >/dev/null; then checksum=(sha256sum); fi
fetch() {
  [[ -f $2 ]] || curl -fsSL --retry 3 "$1" -o "$2"
  printf '%s  %s\n' "$3" "$2" | "${checksum[@]}" -c -
}
fetch https://raw.githubusercontent.com/facil-io/cstl/24a57015d0989c64d5cc08ea9d24bd6cf97848bb/fio-stl.h \
  .deps/fio-stl.h fa05bdd4c193cf77cc1791f4e5a0c356c56e7abba38af284786231f0d0082154
fetch https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip \
  .deps/sqlite.zip 1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
sqlite=.deps/sqlite-amalgamation-3530400
[[ -f $sqlite/sqlite3.c ]] || unzip -q .deps/sqlite.zip -d .deps
flags=(-O3 -DNDEBUG -std=gnu11 -pthread -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION)
# SQLite changes less often than the application; cache its compiled object.
if [[ ! -f .deps/sqlite3.o || build.sh -nt .deps/sqlite3.o || $sqlite/sqlite3.c -nt .deps/sqlite3.o ]]; then
  "${CC:-cc}" "${flags[@]}" -c "$sqlite/sqlite3.c" -o .deps/sqlite3.o
fi
"${CC:-cc}" "${flags[@]}" -I.deps -I"$sqlite" server.c .deps/sqlite3.o -lm -o bin/server
