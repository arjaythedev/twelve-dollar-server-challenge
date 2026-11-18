#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-gcc}
PGO=${PGO:-1}
URL=https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
ZIP=vendor/sqlite-amalgamation-3540100.zip
SRC=(src/sqlite_layer.c src/protocol.c src/uring_net.c src/uring_rings.c src/main.c)
OBJ=(pgo/sqlite_layer.o pgo/protocol.o pgo/uring_net.o pgo/uring_rings.o pgo/main.o)
CFLAGS=(-std=gnu11 -O3 -march=x86-64-v3 -mtune=cascadelake -mprefer-vector-width=256 -flto
  -Ivendor/sqlite-amalgamation-3540100 -Isrc
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_DEPRECATED
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE
  -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_COMPLETE
  -DSQLITE_OMIT_JSON -DSQLITE_TEMP_STORE=3 -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1 -DNDEBUG
  -Wno-unused-parameter -Wno-unused-function)
LIBS=(-lpthread -lm -ldl)

build() {
  local out=$1
  shift
  local i
  for i in "${!SRC[@]}"; do
    "$CC" "${CFLAGS[@]}" "$@" -c "${SRC[i]}" -o "${OBJ[i]}" || return 1
  done
  "$CC" "${CFLAGS[@]}" "$@" "${OBJ[@]}" -o "$out" "${LIBS[@]}"
}

train() {
  rm -rf pgo/prof
  mkdir -p pgo/prof
  rm -f pgo/train.db pgo/train.db-wal pgo/train.db-shm
  if [ -f ../../schema.sql ]; then
    sqlite3 pgo/train.db < ../../schema.sql || return 1
  else
    sqlite3 pgo/train.db <<'SQL' || return 1
CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));
CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));
CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));
CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
CREATE INDEX posts_user_id_idx ON posts (user_id);
CREATE INDEX likes_post_id_idx ON likes (post_id);
SQL
  fi
  sqlite3 pgo/train.db <<'SQL' || return 1
BEGIN;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<200) INSERT INTO users (id, username) SELECT i, 'trainer'||i FROM n;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<1000) INSERT INTO posts (id, user_id, body, created_at) SELECT i, 1+(i%200), 'synthetic seed post '||i, strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-'||(1000-i)||' seconds') FROM n;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<500) INSERT OR IGNORE INTO likes (user_id, post_id) SELECT 1+(i%200), 1+(i%1000) FROM n;
COMMIT;
SQL
  build pgo/server-prof -fprofile-generate=pgo/prof || return 1
  local p=45555
  while (exec 9<>/dev/tcp/127.0.0.1/$p) 2>/dev/null; do p=$((p + 1)); done
  SQLITE_PATH=pgo/train.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=$p pgo/server-prof >pgo/server.log 2>&1 &
  local pid=$!
  trap 'kill -INT "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' EXIT
  for i in {1..100}; do curl -fsS "http://127.0.0.1:$p/health" >/dev/null 2>&1 && break; [ "$i" -lt 100 ] || return 1; sleep .05; done
  bin/train "$p" >pgo/train.log 2>&1 || return 1
  kill -INT "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  trap - EXIT
  [ -n "$(find pgo/prof -name '*.gcda' -print -quit)" ]
}

mkdir -p vendor bin pgo
[ -f "$ZIP" ] || curl -fL --retry 3 -o "$ZIP" "$URL"
echo "$SHA  $ZIP" | sha256sum -c -
unzip -q -o "$ZIP" -d vendor
rm -f bin/server
if [ "$PGO" != 0 ] && "$CC" -std=gnu11 -O2 -Wall -Wextra -o bin/train src/train.c && train; then
  build bin/server -fprofile-use=pgo/prof -fprofile-correction -fprofile-partial-training -Wno-missing-profile || rm -f bin/server
fi
[ -x bin/server ] || build bin/server
