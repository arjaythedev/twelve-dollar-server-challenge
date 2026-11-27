#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[[ ! -x /opt/erlang/29.1.1/bin/erl ]] || export PATH="/opt/erlang/29.1.1/bin:$PATH"
[[ ! -x /opt/elixir/1.20.5/bin/elixirc ]] || export PATH="/opt/elixir/1.20.5/bin:$PATH"
mkdir -p .deps bin
checksum=(shasum -a 256)
if command -v sha256sum >/dev/null; then checksum=(sha256sum); fi
fetch() {
  [[ -f $2 ]] || curl -fsSL --retry 3 "$1" -o "$2"
  printf '%s  %s\n' "$3" "$2" | "${checksum[@]}" -c -
}
# esqlite (Erlang NIF binding, source) + the SQLite amalgamation it is built against
fetch https://codeload.github.com/mmzeeman/esqlite/tar.gz/58454af87559981aed6fb1235fc3f63d618505db .deps/esqlite.tar.gz \
  d4ed2e3fd0a79189f9f163054aa6e51d8bf6b6efd200db929d166e9f4970e84f
if [[ ! -d .deps/esqlite/src ]]; then
  mkdir -p .deps/esqlite && tar -xzf .deps/esqlite.tar.gz --strip-components=1 -C .deps/esqlite
fi
fetch https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip .deps/sqlite.zip \
  1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
sqlite=.deps/sqlite-amalgamation-3530400
[[ -f $sqlite/sqlite3.c ]] || unzip -q .deps/sqlite.zip -d .deps
mkdir -p .deps/esqlite/ebin
cp .deps/esqlite/src/esqlite.app.src .deps/esqlite/ebin/esqlite.app
if [[ ! -f .deps/esqlite/.compiled || build.sh -nt .deps/esqlite/.compiled ]]; then
  erlc +no_debug_info -I .deps/esqlite/include -o .deps/esqlite/ebin .deps/esqlite/src/*.erl
  touch .deps/esqlite/.compiled
fi
include=$(erl -noshell -eval 'io:format("~s/usr/include", [code:root_dir()]), halt().')
nif=.deps/esqlite/priv/esqlite3_nif.so
if [[ ! -f $nif || build.sh -nt $nif || $sqlite/sqlite3.c -nt $nif ]]; then
  mkdir -p .deps/esqlite/priv
  link=(-shared)
  [[ $(uname -s) != Darwin ]] || link=(-dynamiclib -undefined dynamic_lookup)
  # Run sqlite3_step on the normal scheduler: with everything in the page cache or mmap a step takes
  # microseconds, so the hop to a dirty-IO thread (two OS threads on one core) costs more than the step.
  sed 's/{"step", 1, esqlite_step, ERL_NIF_DIRTY_JOB_IO_BOUND}/{"step", 1, esqlite_step, 0}/' \
    .deps/esqlite/c_src/esqlite3_nif.c > .deps/esqlite/c_src/esqlite3_nif_patched.c
  grep -q '{"step", 1, esqlite_step, 0}' .deps/esqlite/c_src/esqlite3_nif_patched.c
  "${CC:-cc}" -O3 -DNDEBUG -std=gnu11 -fPIC -pthread \
    -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -I"$include" -I"$sqlite" -I.deps/esqlite/c_src \
    .deps/esqlite/c_src/esqlite3_nif_patched.c "$sqlite/sqlite3.c" "${link[@]}" -lm -o "$nif"
fi
elixirc -pa .deps/esqlite/ebin -o bin lib/*.ex
