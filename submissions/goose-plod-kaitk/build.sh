#!/usr/bin/env bash
# Builds bin/server as a normal user. goose build fetches Plod, goose-http,
# goose-hash and goose-sqlite at the tags server.goose declares, writes the
# program as C, and compiles it with SQLite's amalgamation, which goose-sqlite
# carries. SQLite is built from a profile: train.goose, built with SQLite
# instrumented, serves the routes on a scratch database, and the server's
# build compiles SQLite with what it recorded (pgo-gcc).
set -euo pipefail
cd "$(dirname "$0")"
rm -rf bin/pgo
mkdir -p bin/pgo/generate bin/pgo/use bin/pgo/profile
cp pgo-gcc bin/pgo/generate/gcc
cp pgo-gcc bin/pgo/use/gcc
goose build --cc "$PWD/bin/pgo/generate/gcc" -o bin/pgo/train train.goose
db=$(mktemp -d)
SQLITE_PATH=$db/train.db bin/pgo/train
rm -rf "$db"
find bin/pgo/profile -name '*.gcda' -exec mv {} bin/pgo/profile/sqlite3.gcda \;
goose build --cc "$PWD/bin/pgo/use/gcc" -o bin/server server.goose
