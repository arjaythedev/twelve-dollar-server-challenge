#!/usr/bin/env bash
# Builds bin/server as a normal user: pinned libobjc2, liburing and SQLite from
# source, then the Objective-C server. Everything is compiled with clang 23.
set -euo pipefail
cd "$(dirname "$0")"

LLVM_MAJOR=23
CC=${CC:-clang-$LLVM_MAJOR}
CXX=${CXX:-clang++-$LLVM_MAJOR}
JOBS=$(nproc 2>/dev/null || echo 2)

LIBOBJC2_VER=2.3
LIBOBJC2_SHA=5ead2276b42a534ac40437ce53b2231320b985539dc325453d93874be8d92869
ROBINMAP_VER=1.4.0
ROBINMAP_SHA=7930dbf9634acfc02686d87f615c0f4f33135948130b8922331c16d90a03250c
LIBURING_VER=2.15
LIBURING_SHA=8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a
SQLITE_VER=3540100
SQLITE_YEAR=2026
SQLITE_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

DEPS=deps
BUILD=build
log() { echo "build: $*"; }
fetch() { # fetch <url> <dest> <sha256>
    if [ ! -f "$2" ]; then log "downloading $(basename "$2")"; curl -fsSL "$1" -o "$2.part" && mv "$2.part" "$2"; fi
    echo "$3  $2" | sha256sum -c - >/dev/null || { echo "sha256 mismatch: $2" >&2; rm -f "$2"; exit 1; }
}

mkdir -p "$DEPS" "$BUILD" bin
fetch "https://github.com/gnustep/libobjc2/archive/refs/tags/v$LIBOBJC2_VER.tar.gz" "$DEPS/libobjc2-$LIBOBJC2_VER.tar.gz" "$LIBOBJC2_SHA"
fetch "https://github.com/Tessil/robin-map/archive/refs/tags/v$ROBINMAP_VER.tar.gz" "$DEPS/robin-map-$ROBINMAP_VER.tar.gz" "$ROBINMAP_SHA"
fetch "https://github.com/axboe/liburing/archive/refs/tags/liburing-$LIBURING_VER.tar.gz" "$DEPS/liburing-$LIBURING_VER.tar.gz" "$LIBURING_SHA"
fetch "https://sqlite.org/$SQLITE_YEAR/sqlite-amalgamation-$SQLITE_VER.zip" "$DEPS/sqlite-amalgamation-$SQLITE_VER.zip" "$SQLITE_SHA"

# libobjc2 (GNUstep Objective-C runtime): its standard shared build, loaded via rpath.
OBJC_PREFIX="$PWD/$DEPS/objc"
if [ ! -f "$OBJC_PREFIX/lib/libobjc.so" ]; then
    log "building libobjc2 $LIBOBJC2_VER"
    rm -rf "$DEPS/libobjc2-$LIBOBJC2_VER" "$DEPS/robin-map-$ROBINMAP_VER" "$DEPS/objc-build"
    tar xzf "$DEPS/libobjc2-$LIBOBJC2_VER.tar.gz" -C "$DEPS"
    tar xzf "$DEPS/robin-map-$ROBINMAP_VER.tar.gz" -C "$DEPS"
    cmake -S "$DEPS/libobjc2-$LIBOBJC2_VER" -B "$DEPS/objc-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_ASM_COMPILER="$CC" \
        -DCMAKE_C_FLAGS="-march=native" -DCMAKE_CXX_FLAGS="-march=native" \
        -DCMAKE_INSTALL_PREFIX="$OBJC_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DFETCHCONTENT_SOURCE_DIR_ROBINMAP="$PWD/$DEPS/robin-map-$ROBINMAP_VER" -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
        -DTESTS=OFF -DLEGACY_COMPAT=OFF -DEMBEDDED_BLOCKS_RUNTIME=ON >/dev/null
    cmake --build "$DEPS/objc-build" -j"$JOBS" >/dev/null
    cmake --install "$DEPS/objc-build" >/dev/null
fi

# liburing
URING_PREFIX="$PWD/$DEPS/uring"
if [ ! -f "$URING_PREFIX/lib/liburing.a" ]; then
    log "building liburing $LIBURING_VER"
    rm -rf "$DEPS/liburing-liburing-$LIBURING_VER"
    tar xzf "$DEPS/liburing-$LIBURING_VER.tar.gz" -C "$DEPS"
    (cd "$DEPS/liburing-liburing-$LIBURING_VER" && CC=$CC ./configure --prefix="$URING_PREFIX" >/dev/null &&
     make -j"$JOBS" -s -C src install >/dev/null)
fi

# SQLite amalgamation
SQLITE_DIR="$DEPS/sqlite-amalgamation-$SQLITE_VER"
[ -f "$SQLITE_DIR/sqlite3.c" ] || unzip -q -o "$DEPS/sqlite-amalgamation-$SQLITE_VER.zip" -d "$DEPS"
log "deps ready"

# One-line SQLite patch (see README): when balance_nonroot sorts the pages of a
# split it parks one page under a temporary page number. Stock SQLite uses the
# 1 GB "pending byte" page (262145), which raises the page cache's largest-key
# mark so the next commit's cache truncate scans the whole page hash table.
# nPage+1 is just as unused and keeps that truncate to one hash slot.
mkdir -p "$BUILD"
SQLITE_SRC="$BUILD/sqlite3.c"
sed 's|Pgno pgnoTemp = (PENDING_BYTE/pBt->pageSize)+1;|Pgno pgnoTemp = pBt->nPage+1; /* objc-uring patch */|' \
    "$SQLITE_DIR/sqlite3.c" > "$SQLITE_SRC"
if [ "$(grep -c 'objc-uring patch' "$SQLITE_SRC")" != 1 ]; then
    log "SQLite patch did not apply; building stock SQLite"
    SQLITE_SRC="$SQLITE_DIR/sqlite3.c"
fi

# One SQLite connection, used by one thread at a time under the server's mutex.
SQLITE_FLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
 -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 \
 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DHAVE_FDATASYNC=1 -DSQLITE_USE_ALLOCA"
OPT="-O3 -march=native -flto=full -fno-plt"
OBJC_FLAGS="-fobjc-runtime=gnustep-2.2 -fno-objc-arc -fobjc-exceptions -I$OBJC_PREFIX/include"
INC="-I$SQLITE_DIR -I$URING_PREFIX/include"
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-objc-root-class"

PROFDATA=llvm-profdata-$LLVM_MAJOR
compile_all() { # compile_all <extra flags>
    rm -f "$BUILD"/*.o
    $CC $OPT $SQLITE_FLAGS $1 -I"$SQLITE_DIR" -c "$SQLITE_SRC" -o "$BUILD/sqlite3.o"
    for f in src/*.m; do
        $CC $OPT $OBJC_FLAGS $INC $WARN $1 -c "$f" -o "$BUILD/$(basename "${f%.m}").o"
    done
}
link_server() { # link_server <extra flags>
    $CC $OPT $1 -fuse-ld=lld-$LLVM_MAJOR -o bin/server "$BUILD"/*.o \
        -L"$OBJC_PREFIX/lib" -lobjc -Wl,-rpath,"\$ORIGIN/../deps/objc/lib" "$URING_PREFIX/lib/liburing.a" -lpthread -lm
}

# PGO: an instrumented trainer drives the store/JSON/JWT code on a synthetic
# database (pgo/train.m, no seed data), then everything is rebuilt with the profile.
pgo_build() (
    set -euo pipefail
    rm -rf "$BUILD/prof" "$BUILD/train.db"*
    GEN="-fprofile-generate=$PWD/$BUILD/prof"
    compile_all "$GEN"
    $CC $OPT $OBJC_FLAGS $INC $WARN $GEN -o "$BUILD/train" pgo/train.m \
        "$BUILD/sqlite3.o" "$BUILD/store.o" "$BUILD/util.o" "$BUILD/auth.o" -L"$OBJC_PREFIX/lib" -lobjc \
        -Wl,-rpath,"$OBJC_PREFIX/lib" -fuse-ld=lld-$LLVM_MAJOR -lpthread -lm
    "$BUILD/train" "$BUILD/train.db" 300000
    $PROFDATA merge -o "$BUILD/prof.profdata" "$BUILD"/prof/*.profraw
    compile_all "-fprofile-use=$PWD/$BUILD/prof.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin"
    link_server "-fprofile-use=$PWD/$BUILD/prof.profdata"
    rm -f "$BUILD/train.db"*
)
set +e; pgo_build; PGO_RC=$?; set -e
if [ $PGO_RC = 0 ]; then log "built bin/server (PGO)"
else
    log "PGO failed; plain build"
    compile_all ""
    link_server ""
    log "built bin/server"
fi
