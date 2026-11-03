#!/usr/bin/env bash
# Builds bin/server: liburing + SQLite amalgamation (PGO) + the server (PGO).
# Any PGO step that fails falls back to a plain -O3 build.
set -euo pipefail
cd "$(dirname "$0")"

SRC=src
BUILD=build
DEPS=deps
PROF="$DEPS/prof"

SQLITE_VER=3540000
SQLITE_YEAR=2026
SQLITE_URL="https://sqlite.org/$SQLITE_YEAR/sqlite-amalgamation-$SQLITE_VER.zip"
SQLITE_SHA=68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d
LIBURING_VER=2.15
LIBURING_URL="https://github.com/axboe/liburing/archive/refs/tags/liburing-$LIBURING_VER.tar.gz"
LIBURING_SHA=8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a

# Compile flags. -march=native: the challenge box builds and runs on the same
# machine. -fprofile-update=atomic: worker threads run profiled code.
SQLITE_CFLAGS="-O3 -march=native -fomit-frame-pointer -DSQLITE_THREADSAFE=2 \
 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION \
 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK \
 -DHAVE_FDATASYNC=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 \
 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_CACHE_SIZE=-131072"
SERVER_CFLAGS="-O3 -march=native -fomit-frame-pointer -flto"
SQLITE_SRC="$DEPS/sqlite-amalgamation-$SQLITE_VER/sqlite3.c"
URING_PREFIX="$DEPS/uring"

CC=${CC:-cc}
JOBS=$(nproc 2>/dev/null || echo 2)

log() { echo "build: $*"; }

fetch() { # fetch <url> <dest> <sha256>
    if [ ! -f "$2" ]; then
        log "downloading $(basename "$2")"
        curl -fsSL "$1" -o "$2"
    fi
    echo "$3  $2" | sha256sum -c - >/dev/null || { echo "sha256 mismatch: $2" >&2; exit 1; }
}

# ---------------------------------------------------------------- deps

mkdir -p "$DEPS" "$BUILD" bin
fetch "$SQLITE_URL" "$DEPS/sqlite-amalgamation-$SQLITE_VER.zip" "$SQLITE_SHA"
fetch "$LIBURING_URL" "$DEPS/liburing-$LIBURING_VER.tar.gz" "$LIBURING_SHA"

if [ ! -f "$SQLITE_SRC" ]; then
    unzip -q -o "$DEPS/sqlite-amalgamation-$SQLITE_VER.zip" -d "$DEPS"
fi
if [ ! -f "$URING_PREFIX/lib/liburing.a" ]; then
    tar xzf "$DEPS/liburing-$LIBURING_VER.tar.gz" -C "$DEPS"
    (cd "$DEPS/liburing-liburing-$LIBURING_VER" &&
     CC=$CC ./configure --prefix="$PWD/../uring" >/dev/null &&
     make -j"$JOBS" -s -C src install) >/dev/null
fi
log "deps ready"

SERVER_SRCS="$SRC/server.c $SRC/http.c $SRC/db.c $SRC/json.c $SRC/jwt.c $SRC/util.c"
COMMON_INC="-I$DEPS/sqlite-amalgamation-$SQLITE_VER -I$URING_PREFIX/include"

# the training load generator (not instrumented, not part of the server)
$CC -O2 -D_GNU_SOURCE -I"$SRC" -o "$DEPS/gen" pgo/gen.c "$SRC/jwt.c" "$SRC/json.c" "$SRC/util.c" -lm

# ---------------------------------------------------------------- plain build

plain_build() {
    log "plain build (no PGO)"
    rm -rf "$BUILD"
    mkdir -p "$BUILD"
    $CC $SQLITE_CFLAGS -c "$SQLITE_SRC" -o "$BUILD/sqlite3.o"
    for f in $SERVER_SRCS; do
        $CC $SERVER_CFLAGS $COMMON_INC -c "$f" -o "$BUILD/$(basename "${f%.c}").o"
    done
    $CC $SERVER_CFLAGS -o bin/server "$BUILD"/*.o "$URING_PREFIX/lib/liburing.a" -lpthread -lm
    log "built bin/server (plain)"
}

# ---------------------------------------------------------------- PGO build

pgo_build() (
    set -euo pipefail
    log "PGO build"
    rm -rf "$BUILD" "$PROF"
    mkdir -p "$BUILD" "$PROF"

    GEN="-fprofile-generate=$PROF -fprofile-update=atomic"
    USE="-fprofile-use=$PROF -fprofile-correction"

    # stage 1: instrumented build of sqlite and the server
    $CC $SQLITE_CFLAGS $GEN -c "$SQLITE_SRC" -o "$BUILD/sqlite3.o"
    for f in $SERVER_SRCS; do
        $CC $SERVER_CFLAGS $GEN $COMMON_INC -c "$f" -o "$BUILD/$(basename "${f%.c}").o"
    done
    $CC $GEN -o "$BUILD/train" pgo/train.c "$BUILD/sqlite3.o" -lm
    $CC $GEN $COMMON_INC -o "$BUILD/server_pgo" "$BUILD"/*.o \
        "$URING_PREFIX/lib/liburing.a" -lpthread -lm -ldl

    # stage 2: train sqlite on a synthetic database
    "$BUILD/train" "$BUILD/train.db"

    # stage 3: run the server on the synthetic database and drive it
    SQLITE_PATH="$BUILD/train.db" JWT_SECRET=pgo-training-secret HOST=127.0.0.1 PORT=18081 \
        "$BUILD/server_pgo" --pgo 16 >"$BUILD/pgo_server.log" 2>&1 &
    SRV=$!
    cleanup() { kill "$SRV" 2>/dev/null || true; }
    trap cleanup EXIT
    for _ in $(seq 100); do
        curl -sf http://127.0.0.1:18081/health >/dev/null && break
        sleep 0.2
    done
    curl -sf http://127.0.0.1:18081/health >/dev/null || exit 1
    "$DEPS/gen" 18081 11 || exit 1
    wait "$SRV" || true

    cp "$BUILD/train.db" "$DEPS/smoke.db"
    # stage 4: rebuild everything with the profiles
    rm -f "$BUILD"/*.o "$BUILD/train" "$BUILD/server_pgo"
    $CC $SQLITE_CFLAGS $USE -c "$SQLITE_SRC" -o "$BUILD/sqlite3.o"
    for f in $SERVER_SRCS; do
        $CC $SERVER_CFLAGS $USE $COMMON_INC -c "$f" -o "$BUILD/$(basename "${f%.c}").o"
    done
    $CC $SERVER_CFLAGS -o bin/server "$BUILD"/*.o "$URING_PREFIX/lib/liburing.a" -lpthread -lm
    log "built bin/server (PGO)"
)

if pgo_build; then
    :
else
    log "PGO build failed; falling back"
    plain_build
fi

# final smoke test: the binary must start and answer /health
if [ ! -f "$DEPS/smoke.db" ]; then
    $CC -O2 -o "$BUILD/train" pgo/train.c "$BUILD/sqlite3.o" -lm
    "$BUILD/train" "$DEPS/smoke.db" >/dev/null
fi
SQLITE_PATH="$DEPS/smoke.db" JWT_SECRET=smoke HOST=127.0.0.1 PORT=18083 bin/server --pgo 6 \
    >"$BUILD/smoke.log" 2>&1 &
SMOKE_PID=$!
for _ in $(seq 60); do
    if curl -sf http://127.0.0.1:18083/health >/dev/null; then
        kill "$SMOKE_PID" 2>/dev/null || true
        log "smoke test ok"
        ls -la bin/server
        exit 0
    fi
    sleep 0.5
done
kill "$SMOKE_PID" 2>/dev/null || true
echo "smoke test FAILED:" >&2
cat "$BUILD/smoke.log" >&2
exit 1
