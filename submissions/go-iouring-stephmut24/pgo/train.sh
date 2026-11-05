#!/usr/bin/env bash
# Best-effort GCC PGO train for the unity SQLite object. Run on Linux with a seed DB:
#   SQLITE_PATH=/path/to/feed.db bash pgo/train.sh
# Then: PGO=1 bash build.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p pgo
: "${SQLITE_PATH:?set SQLITE_PATH to a writable copy of feed.db}"
export PATH="/usr/local/go/bin:${PATH:-}" CGO_ENABLED=1
export CGO_CFLAGS="-O3 -fprofile-generate=pgo -I${PWD} -I${PWD}/csrc \
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_AUTOINIT \
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK \
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_DECLTYPE -DSQLITE_USE_ALLOCA \
  -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_MAX_MMAP_SIZE=0"
go build -trimpath -o bin/server.pgo .
JWT_SECRET="${JWT_SECRET:-twelve-dollar-challenge}" HOST=127.0.0.1 PORT=3099 ./bin/server.pgo &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
for _ in $(seq 30); do curl -sf http://127.0.0.1:3099/health >/dev/null && break; sleep 0.2; done
# Mixed traffic approximating bench/load.js ratios.
for i in $(seq 500); do
  curl -sf http://127.0.0.1:3099/feed >/dev/null || true
  curl -sf http://127.0.0.1:3099/posts/$((1 + i % 1000)) >/dev/null || true
done
kill $PID; wait $PID 2>/dev/null || true
trap - EXIT
echo "PGO profiles in pgo/: $(ls pgo/*.gcda 2>/dev/null | wc -l) gcda files"
echo "Rebuild with: PGO=1 bash build.sh"
