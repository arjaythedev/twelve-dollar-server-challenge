#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/wal-vfs-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
cc -O2 -std=gnu11 -pthread -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_AUTOINIT \
  -Isrc -Isrc/vendor/sqlite-3.54.1 \
  tests/wal_vfs_test.c src/wal_vfs.c src/vendor/sqlite-3.54.1/sqlite3.c \
  -o "$test_dir/wal_vfs_test"
"$test_dir/wal_vfs_test"
