#!/usr/bin/env bash
# Builds the server as a normal user: fetches the pinned SQLite amalgamation and liburing (both
# SHA-256 checked), builds liburing as a static library, then builds the package in release mode.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/swift/usr/bin:$PATH"

fetch() { # fetch <url> <file> <sha256>
    [ -f "$2" ] || curl -fsSL --retry 3 -o "$2" "$1"
    echo "$3  $2" | sha256sum -c - >/dev/null
}

mkdir -p vendor .deps
SQ=sqlite-amalgamation-3540100
if [ ! -f vendor/sqlite3.c ]; then
    fetch "https://www.sqlite.org/2026/$SQ.zip" vendor/$SQ.zip \
        0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
    (cd vendor && unzip -qo $SQ.zip && mv $SQ/sqlite3.c $SQ/sqlite3.h . && rm -rf $SQ $SQ.zip)
fi

URING_VER=2.15
URING="$PWD/.deps/uring"
if [ ! -f "$URING/lib/liburing.a" ]; then
    fetch "https://github.com/axboe/liburing/archive/refs/tags/liburing-$URING_VER.tar.gz" .deps/liburing.tar.gz \
        8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a
    tar xzf .deps/liburing.tar.gz -C .deps
    (cd ".deps/liburing-liburing-$URING_VER" && ./configure --prefix="$URING" >/dev/null &&
        make -j"$(nproc)" -s -C src install >/dev/null)
fi

swift build -c release --product feed-server --static-swift-stdlib \
    -Xlinker "$URING/lib/liburing.a"
