#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
openssl_version="$(pkg-config --modversion openssl)"
if [[ "$openssl_version" != 3.0.13 ]]; then
  printf 'Expected Ubuntu 24.04 OpenSSL 3.0.13, found %s\n' "$openssl_version" >&2
  exit 1
fi
bash deps.sh
mkdir -p bin
CC="${CC:-cc}"
flags=(-std=c11 -O3 -DNDEBUG -march=x86-64-v3 -flto=1 -fno-plt -pthread
  -isystem .deps -isystem .deps/sqlite -isystem .deps/liburing/src/include)
if [[ "${SANITIZE:-0}" == 1 ]]; then
  flags=(-std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -pthread
    -isystem .deps -isystem .deps/sqlite -isystem .deps/liburing/src/include)
fi
sqlite_flags=(-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0)
config="$("$CC" --version | head -1) ${flags[*]} ${sqlite_flags[*]} $(sha256sum deps.lock sqlite_driver.c native_db.h)"
if [[ ! -f bin/build-config || "$(cat bin/build-config)" != "$config" ]]; then
  rm -f bin/sqlite.o bin/yyjson.o
  printf '%s\n' "$config" > bin/build-config
fi
if [[ ! -f bin/sqlite.o || .deps/sqlite/sqlite3.c -nt bin/sqlite.o ]]; then
  "$CC" "${flags[@]}" -w "${sqlite_flags[@]}" -c sqlite_driver.c -o bin/sqlite.o
fi
if [[ ! -f bin/yyjson.o || .deps/yyjson.c -nt bin/yyjson.o ]]; then
  "$CC" "${flags[@]}" -w -DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_UTILS=1 \
    -c .deps/yyjson.c -o bin/yyjson.o
fi
"$CC" "${flags[@]}" -Wall -Wextra -Wshadow -Wconversion -Wno-deprecated-declarations \
  -DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_UTILS=1 server.c api.c \
  bin/yyjson.o bin/sqlite.o .deps/liburing/src/liburing.a -lcrypto -lm -ldl -o bin/server.new
mv -f bin/server.new bin/server
