#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[[ ! -x /opt/erlang/29.1.1/bin/erl ]] || export PATH="/opt/erlang/29.1.1/bin:$PATH"
export ERL_FLAGS='+S 1:1 +SDcpu 1:1 +SDio 1 +A 1'
erl -noshell -eval 'true = list_to_integer(erlang:system_info(otp_release)) >= 27, halt().'
mkdir -p .deps bin
checksum=(shasum -a 256)
if command -v sha256sum >/dev/null; then checksum=(sha256sum); fi
fetch() {
  [[ -f $2 ]] || curl -fsSL --retry 3 "$1" -o "$2"
  printf '%s  %s\n' "$3" "$2" | "${checksum[@]}" -c -
}
dependency() {
  fetch "https://codeload.github.com/$2/tar.gz/$3" ".deps/$1.tar.gz" "$4"
  if [[ ! -d .deps/$1/src ]]; then
    mkdir -p ".deps/$1"
    tar -xzf ".deps/$1.tar.gz" --strip-components=1 -C ".deps/$1"
  fi
}
dependency cowboy ninenines/cowboy 79e3fb02b31d47af6e69e8f3ba18fba291a3072a \
  49afde6f8211761c08fea5b29a7f14caea7456b0e67a2d98020126a93d1cc0b9
dependency cowlib ninenines/cowlib 69a047e5fff0232b27c347a64a644f7185638488 \
  53e4a1d7d77eb78645a239a2cd7c58ad851897f241a954369acbaf2b4d9868ef
dependency ranch ninenines/ranch 616ce1566986ee704b9be36445a144f6c1c9c40c \
  51c4cadb0641a09f654a24e93fa74d5ccce32cecc4cbb58e98165b9b522488a6
dependency esqlite mmzeeman/esqlite 58454af87559981aed6fb1235fc3f63d618505db \
  d4ed2e3fd0a79189f9f163054aa6e51d8bf6b6efd200db929d166e9f4970e84f
fetch https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip .deps/sqlite.zip \
  1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
sqlite=.deps/sqlite-amalgamation-3530400
[[ -f $sqlite/sqlite3.c ]] || unzip -q .deps/sqlite.zip -d .deps
export ERL_LIBS="$PWD/.deps"
for name in cowlib ranch cowboy esqlite; do
  mkdir -p ".deps/$name/ebin"
  [[ ! -f .deps/$name/src/$name.app.src ]] || cp ".deps/$name/src/$name.app.src" ".deps/$name/ebin/$name.app"
  if [[ ! -f .deps/$name/.compiled || build.sh -nt .deps/$name/.compiled ]]; then
    erlc +no_debug_info -I ".deps/$name/include" -o ".deps/$name/ebin" ".deps/$name/src/"*.erl
    touch ".deps/$name/.compiled"
  fi
done
include=$(erl -noshell -eval 'io:format("~s/usr/include", [code:root_dir()]), halt().')
nif=.deps/esqlite/priv/esqlite3_nif.so
if [[ ! -f $nif || build.sh -nt $nif || $sqlite/sqlite3.c -nt $nif ]]; then
  mkdir -p .deps/esqlite/priv
  link=(-shared)
  [[ $(uname -s) != Darwin ]] || link=(-dynamiclib -undefined dynamic_lookup)
  "${CC:-cc}" -O3 -DNDEBUG -std=gnu11 -fPIC -pthread \
    -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -I"$include" -I"$sqlite" \
    .deps/esqlite/c_src/esqlite3_nif.c "$sqlite/sqlite3.c" "${link[@]}" -lm -o "$nif"
fi
erlc -Werror +no_debug_info -o bin ./*.erl
