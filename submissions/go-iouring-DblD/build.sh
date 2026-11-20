#!/usr/bin/env bash
# Runs as a normal user. The only dependency is SQLite itself: the official amalgamation,
# pinned by version and sha256. It goes in sqlite/ and db.c includes it, so cgo compiles it
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
# (#264) build their SQLite the same way. (cgo accepts only some of these flags in a #cgo
# line, so they are given here. Without this script, cgo's default -O2 applies.)
CFLAGS="-O3 -march=native -mprefer-vector-width=256 -fno-plt -fno-semantic-interposition -fomit-frame-pointer"
CFLAGS="$CFLAGS -fno-stack-protector -fno-stack-clash-protection -U_FORTIFY_SOURCE"
case "$(uname -m)" in x86_64) CFLAGS="$CFLAGS -fcf-protection=none" ;; esac
# Go decides whether db.c needs compiling again from db.c and these flags; it does not look
# at sqlite/sqlite3.c, which db.c includes. Naming the pinned release in a flag makes a
# change of the pin rebuild it (found the hard way: a new pin kept linking the old SQLite).
export CGO_CFLAGS="$CFLAGS -DPINNED_SQLITE=$SQLITE"
go build -trimpath -ldflags='-s -w' -o bin/server .
