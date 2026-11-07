#!/usr/bin/env bash
# Builds the app as a normal user: fetches the pinned odin-http commit (and applies our pooled
# memory patch) and the pinned SQLite amalgamation, then compiles everything with Odin.
set -euo pipefail
cd "$(dirname "$0")"

ODIN_HTTP_REV=fac113fbd828aad3d71479a534b5de4358b6a07b
SQLITE_VERSION=3530400
SQLITE_SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d

mkdir -p .deps bin

# odin-http at the pinned commit, reset to pristine before patching so reruns are idempotent.
HTTP=.deps/odin-http
if [ ! -d "$HTTP/.git" ]; then
    rm -rf "$HTTP"
    git clone --quiet https://github.com/laytan/odin-http.git "$HTTP"
fi
git -C "$HTTP" cat-file -e "$ODIN_HTTP_REV^{commit}" 2>/dev/null || git -C "$HTTP" fetch --quiet origin
git -C "$HTTP" reset --quiet --hard "$ODIN_HTTP_REV"
git -C "$HTTP" clean --quiet -fdx
git -C "$HTTP" apply "$PWD/patches/odin-http-pool.patch"

# SQLite amalgamation, checksum verified, built single-threaded (one event-loop thread).
SQLITE_ZIP=.deps/sqlite-amalgamation-$SQLITE_VERSION.zip
SQLITE_DIR=.deps/sqlite-amalgamation-$SQLITE_VERSION
[ -f "$SQLITE_ZIP" ] || curl -fsSL --retry 3 "https://www.sqlite.org/2026/sqlite-amalgamation-$SQLITE_VERSION.zip" -o "$SQLITE_ZIP"
echo "$SQLITE_SHA256  $SQLITE_ZIP" | sha256sum -c --quiet -
[ -f "$SQLITE_DIR/sqlite3.c" ] || unzip -q -o "$SQLITE_ZIP" -d .deps
sqlite_build() {
    cc -O3 -march=native -DNDEBUG \
        -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
        -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_SHARED_CACHE \
        -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_USE_ALLOCA -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
        -DSQLITE_MAX_EXPR_DEPTH=0 \
        "$@" -c "$SQLITE_DIR/sqlite3.c" -o .deps/sqlite3.o
    rm -f .deps/libsqlite3.a
    ar rcs .deps/libsqlite3.a .deps/sqlite3.o
}
# Profile-guided: an instrumented SQLite runs the server's own statements against a synthetic
# database (pgo.odin), then SQLite is rebuilt with that profile. The profile depends on the queries.
if [ ! -f .deps/libsqlite3.a ] || [ -n "$(find build.sh db.odin pgo.odin -newer .deps/libsqlite3.a)" ]; then
    rm -f .deps/sqlite3.gcda .deps/pgo-train.db*
    sqlite_build -fprofile-generate
    odin build . -collection:http="$HTTP" -define:PGO_TRAIN=true \
        -extra-linker-flags:"$(cc -print-file-name=libgcov.a)" -out:.deps/pgo-train
    SQLITE_PATH=.deps/pgo-train.db .deps/pgo-train
    sqlite_build -fprofile-use -fprofile-partial-training -Werror=missing-profile
    rm -f .deps/pgo-train .deps/pgo-train.db*
fi

odin build . -collection:http="$HTTP" -o:speed -microarch:native -no-bounds-check -disable-assert -out:bin/server
