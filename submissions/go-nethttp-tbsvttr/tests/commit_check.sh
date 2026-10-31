#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source ./sqlite-build.sh
cc -O2 -I"$sqlite" -c tests/commit_fault.c -o .deps/commit_fault.o
export PATH="/opt/twelve-go-1.27.1/bin:/usr/local/go/bin:$PATH" CGO_ENABLED=1 GOTOOLCHAIN=local
go build -mod=readonly -tags 'libsqlite3 sqlite_omit_load_extension' -ldflags="-extldflags '$PWD/.deps/commit_fault.o -Wl,--wrap=sqlite3_open_v2'" -o bin/commit-test .
python3 tests/commit_check.py
