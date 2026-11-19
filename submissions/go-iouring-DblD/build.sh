#!/usr/bin/env bash
# Runs as a normal user. The only dependency is SQLite itself: the official amalgamation,
# pinned by version and sha256. It goes in sqlite/ and db.c includes it, compiled
# once, as part of db.c.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/go/bin:$PATH" CGO_ENABLED=1

SQLITE=sqlite-amalgamation-3540100
SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
if [ ! -f sqlite/sqlite3.c ]; then
  mkdir -p sqlite
  curl -fsSL --retry 3 "https://www.sqlite.org/2026/$SQLITE.zip" -o sqlite.zip
  printf '%s  %s\n' "$SHA256" sqlite.zip | sha256sum -c -
  unzip -q -o -j sqlite.zip "$SQLITE/sqlite3.c" "$SQLITE/sqlite3.h" -d sqlite
  rm -f sqlite.zip
fi
# Compiler flags for db.c and the SQLite inside it. The server is built on the box it runs
# on, so -march=native is that box's CPU. Ubuntu's gcc adds a stack canary, checked memcpy
# calls, stack probes and branch-tracking markers to every function by default; they cost
# instructions in the B-tree functions that run a dozen times per request, and are turned
# off here for this one C file. go-iouring-davidefrageri (#354) and rust-iouring-kensac
# (#264) build their SQLite the same way.
CFLAGS="-O3 -march=native -mprefer-vector-width=256 -fno-plt -fno-semantic-interposition -fomit-frame-pointer"
CFLAGS="$CFLAGS -fno-stack-protector -fno-stack-clash-protection -U_FORTIFY_SOURCE"
case "$(uname -m)" in x86_64) CFLAGS="$CFLAGS -fcf-protection=none" ;; esac
# SQLite's compile-time options.
CFLAGS="$CFLAGS -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_DQS=0 -DHAVE_USLEEP=1"

# db.c is compiled here, not by cgo, into db.syso (the go tool links any .syso file in the
# folder, and rebuilds when its content changes).
compile() { gcc $CFLAGS -pthread "$@" -c db.c -o db.syso; }

# Profile-guided build (allowed since the organiser's ruling of 2026-11-15, rule 10: the
# profile must be recorded by build.sh on the box, during the build, on a scratch database).
# Three steps: compile db.c with counters in it; run the training program (train.go, train.c:
# it makes its own database under pgo/ and sends about 44,000 requests through the server's
# own request functions, no network); compile db.c again with the recorded counts. gcc then
# knows which branch is the usual one and lays the code out for it.
# Any failure, or the training taking over two minutes, ends in the plain build instead.
# PGO=0 bash build.sh skips it.
train() {
  rm -rf pgo && mkdir pgo &&
  compile -fprofile-generate -fprofile-update=prefer-atomic -fprofile-dir="$PWD/pgo" &&
  CGO_LDFLAGS=-fprofile-generate go build -trimpath -tags train -o bin/server.train . &&
  timeout 120 bin/server.train "$PWD/pgo/train.db" &&
  ls pgo/*.gcda > /dev/null
}
if [ "${PGO:-1}" = 1 ] && train && compile -fprofile-use -fprofile-dir="$PWD/pgo" -fprofile-correction -fprofile-partial-training -Wno-missing-profile; then
  echo "build.sh: profile-guided build"
else
  echo "build.sh: plain build"
  compile
fi
rm -rf pgo bin/server.train
go build -trimpath -ldflags='-s -w' -o bin/server .
