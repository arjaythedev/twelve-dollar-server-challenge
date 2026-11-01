#!/usr/bin/env bash
# Builds as an ordinary user. Downloads only checksum-pinned sources (SQLite, liburing).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p obj bin
if [ ! -f obj/sqlite/libsqlite3.so ]; then
  mkdir -p obj/sqlite
  curl -fsSL --retry 3 https://www.sqlite.org/2026/sqlite-autoconf-3530400.tar.gz -o obj/sqlite/source.tar.gz
  echo '0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c  obj/sqlite/source.tar.gz' | sha256sum -c -
  tar -xzf obj/sqlite/source.tar.gz -C obj/sqlite --strip-components=1 sqlite-autoconf-3530400/sqlite3.c sqlite-autoconf-3530400/sqlite3.h
  clang -O2 -fPIC -shared -DSQLITE_THREADSAFE=1 -DHAVE_FDATASYNC=1 obj/sqlite/sqlite3.c -o obj/sqlite/libsqlite3.so -lpthread -ldl -lm
fi
if [ ! -f obj/liburing/src/liburing.a ]; then
  mkdir -p obj/liburing
  curl -fsSL --retry 3 https://github.com/axboe/liburing/archive/refs/tags/liburing-2.15.tar.gz -o obj/liburing.tar.gz
  echo '8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a  obj/liburing.tar.gz' | sha256sum -c -
  tar -xzf obj/liburing.tar.gz -C obj/liburing --strip-components=1
  (cd obj/liburing && CFLAGS='-O2 -fPIC' ./configure --cc=clang && make -C src -j2 liburing.a)
fi
dotnet restore -r linux-x64 --locked-mode
dotnet publish -c Release -r linux-x64 --no-restore --self-contained true -o bin
cp obj/sqlite/libsqlite3.so bin/
clang -std=gnu11 -O2 -fPIC -shared -Wall -Wextra -Werror -Iobj/liburing/src/include \
  uring_server.c obj/liburing/src/liburing.a -o bin/libfeeduring.so
