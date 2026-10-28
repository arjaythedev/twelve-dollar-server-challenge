#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
source ./versions.env
mkdir -p bin obj deps
case $(uname -m) in
  x86_64) event_offset=4; event_bytes=12; event_alignment=4 ;;
  aarch64) event_offset=8; event_bytes=16; event_alignment=8 ;;
  *) echo 'Unsupported Linux architecture' >&2; exit 1 ;;
esac
cat > obj/platform.ads <<EOF
package Platform is
   Epoll_Data_Offset : constant := $event_offset;
   Epoll_Event_Bytes : constant := $event_bytes;
   Epoll_Event_Alignment : constant := $event_alignment;
end Platform;
EOF

# SQLite from the official amalgamation, pinned by SHA-256 (sqlite.org also
# publishes its SHA3-256). Built for one thread, without unused features.
archive=deps/sqlite-autoconf-$SQLITE_SOURCE_VERSION.tar.gz
if ! echo "$SQLITE_SOURCE_SHA256  $archive" | sha256sum -c --quiet - >/dev/null 2>&1; then
  curl -fsSL --retry 3 -o "$archive.part" \
    "https://www.sqlite.org/$SQLITE_SOURCE_YEAR/sqlite-autoconf-$SQLITE_SOURCE_VERSION.tar.gz"
  echo "$SQLITE_SOURCE_SHA256  $archive.part" | sha256sum -c --quiet -
  mv "$archive.part" "$archive"
fi
rm -rf obj/sqlite && mkdir -p obj/sqlite
tar -xzf "$archive" -C obj/sqlite --strip-components=1 \
  "sqlite-autoconf-$SQLITE_SOURCE_VERSION/sqlite3.c" "sqlite-autoconf-$SQLITE_SOURCE_VERSION/sqlite3.h"
sqlite_options=(
  -DSQLITE_THREADSAFE=0            # only the event-loop thread calls SQLite
  -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
  -DSQLITE_MAX_EXPR_DEPTH=0
  -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_PROGRESS_CALLBACK
  -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_USE_ALLOCA
  -DSQLITE_TEMP_STORE=3
  -DHAVE_USLEEP=1                  # millisecond busy waits instead of whole seconds
  -DHAVE_FDATASYNC=1
)
gcc-13 -O2 -w "${sqlite_options[@]}" -c obj/sqlite/sqlite3.c -o obj/sqlite3.o

exec gnatmake-13 -j1 -O3 -gnatn -gnat2022 -gnatwa -gnatVa -fstack-protector-strong \
  -D obj -Iobj -Isrc src/server.adb -o bin/server -largs obj/sqlite3.o -lcrypto -lm
