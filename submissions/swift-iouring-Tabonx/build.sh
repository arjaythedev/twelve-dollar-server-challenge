#!/usr/bin/env bash
# Builds bin/server as a normal user:
#  1. the pinned SQLite amalgamation and liburing 2.15 (both SHA-256 checked) from source,
#  2. SQLite with profile-guided optimization, trained by pgo/train.c on a synthetic database
#     created from ../../schema.sql (no seed data),
#  3. the Swift sources with swiftc, linked statically against the Swift runtime.
set -euo pipefail
cd "$(dirname "$0")"
BIN=/opt/swift/usr/bin
[ -x "$BIN/swiftc" ] || BIN="$(dirname "$(command -v swiftc)")"
DEPS="$PWD/.deps"; OUT="$PWD/.build"; mkdir -p "$DEPS" "$OUT" bin
SCHEMA="${SCHEMA:-$PWD/../../schema.sql}"

fetch() { # fetch <url> <file> <sha256>
    [ -f "$2" ] || curl -fsSL "$1" -o "$2"
    echo "$3  $2" | sha256sum -c - >/dev/null
}
SQLITE_VER=3540000
fetch "https://sqlite.org/2026/sqlite-amalgamation-$SQLITE_VER.zip" "$DEPS/sqlite.zip" \
    68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d
if [ ! -f "$DEPS/sqlite3.c" ]; then
    unzip -q -o "$DEPS/sqlite.zip" -d "$DEPS"
    cp "$DEPS/sqlite-amalgamation-$SQLITE_VER/sqlite3.c" "$DEPS/sqlite3.c"
fi
cp "$DEPS/sqlite-amalgamation-$SQLITE_VER/sqlite3.h" Sources/CSQLite/include/sqlite3.h
URING_VER=2.15
fetch "https://github.com/axboe/liburing/archive/refs/tags/liburing-$URING_VER.tar.gz" "$DEPS/liburing.tar.gz" \
    8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a
URING="$DEPS/uring"
if [ ! -f "$URING/lib/liburing.a" ]; then
    tar xzf "$DEPS/liburing.tar.gz" -C "$DEPS"
    (cd "$DEPS/liburing-liburing-$URING_VER" && CC="$BIN/clang" ./configure --prefix="$URING" >/dev/null &&
     make -j"$(nproc)" -s -C src install >/dev/null)
fi

# Single-threaded SQLite without mmap, deprecated APIs or shared cache.
CFLAGS=(-O3 -march=native -fomit-frame-pointer)
SQLITE_DEFINES=(-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_DQS=0 -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DHAVE_FDATASYNC=1)
CLANG="$BIN/clang"
if ! (rm -rf "$OUT/pgo" && mkdir -p "$OUT/pgo" &&
      "$CLANG" "${CFLAGS[@]}" "${SQLITE_DEFINES[@]}" -fprofile-instr-generate="$OUT/pgo/%p.profraw" \
          -ISources/CSQLite/include "$DEPS/sqlite3.c" pgo/train.c -lm -o "$OUT/pgo/train" &&
      "$OUT/pgo/train" "$SCHEMA" "$OUT/pgo/train.db" 8 &&
      "$BIN/llvm-profdata" merge -o "$OUT/pgo/sqlite.profdata" "$OUT"/pgo/*.profraw &&
      "$CLANG" "${CFLAGS[@]}" "${SQLITE_DEFINES[@]}" -fprofile-instr-use="$OUT/pgo/sqlite.profdata" \
          -Wno-profile-instr-out-of-date -c "$DEPS/sqlite3.c" -o "$OUT/sqlite3.o"); then
    echo "PGO failed, building SQLite without a profile" >&2
    "$CLANG" "${CFLAGS[@]}" "${SQLITE_DEFINES[@]}" -c "$DEPS/sqlite3.c" -o "$OUT/sqlite3.o"
fi
rm -f "$OUT"/pgo/train.db*

"$CLANG" -O2 -ISources/CShim/include -c Sources/CShim/shim.c -o "$OUT/shim.o"
"$CLANG" -O2 -I"$URING/include" -ISources/CUring/include -c Sources/CUring/tu.c -o "$OUT/tu.o"
"$BIN/swiftc" -wmo -Ounchecked -enforce-exclusivity=unchecked -parse-as-library -swift-version 6 -module-name Server \
    -static-stdlib \
    -I Sources/CSQLite/include -I Sources/CShim/include -I Sources/CUring/include \
    -Xcc -ISources/CSQLite/include -Xcc -ISources/CShim/include -Xcc -ISources/CUring/include -Xcc -I"$URING/include" \
    Sources/Server/*.swift "$OUT/shim.o" "$OUT/tu.o" "$OUT/sqlite3.o" "$URING/lib/liburing.a" -o bin/server
echo "built bin/server"
