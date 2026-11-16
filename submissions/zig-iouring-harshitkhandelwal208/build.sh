#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

mkdir -p bin deps src/c

if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  curl -fsSL --retry 3 -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  echo "$SQLITE_SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi

PGO_FLAG=""
PROFDATA_CMD=""
for cmd in llvm-profdata llvm-profdata-20 llvm-profdata-19 llvm-profdata-18 llvm-profdata-17; do
  if command -v "$cmd" >/dev/null 2>&1; then
    PROFDATA_CMD="$cmd"
    break
  fi
done

# Dynamic on-box PGO profile generation (Rule 10 compliant)
if command -v clang >/dev/null 2>&1 && [ -n "$PROFDATA_CMD" ] && [ -f src/c/pgo_train.c ]; then
  echo "== Recording SQLite PGO profile on runner (Rule 10 compliant) =="
  TMP_PGO="$(mktemp -d)"
  if clang -O3 -march=native \
    -I deps/$SQLITE \
    -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1 \
    -DSQLITE_DISABLE_DIRSYNC -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_EXPLAIN \
    -fprofile-generate="$TMP_PGO/prof" \
    src/shim.c src/c/pgo_train.c -lpthread -ldl -lm -o "$TMP_PGO/pgo_train" 2>/dev/null; then

    # Create temporary scratch training schema and run
    sqlite3 "$TMP_PGO/scratch.db" >/dev/null 2>&1 <<'SQL' || true
CREATE TABLE users (
  id          INTEGER PRIMARY KEY,
  username    TEXT NOT NULL UNIQUE,
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE TABLE posts (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users(id),
  body        TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE TABLE likes (
  user_id     INTEGER NOT NULL REFERENCES users(id),
  post_id     INTEGER NOT NULL REFERENCES posts(id),
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  PRIMARY KEY (user_id, post_id)
);
CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
CREATE INDEX posts_user_id_idx       ON posts (user_id);
CREATE INDEX likes_post_id_idx       ON likes (post_id);
PRAGMA journal_mode = WAL;
INSERT INTO users (id, username) VALUES (1, 'user1'), (2, 'user2');
INSERT INTO posts (id, user_id, body) VALUES (1, 1, 'Hello world 1'), (2, 2, 'Hello world 2');
INSERT INTO likes (user_id, post_id) VALUES (1, 2), (2, 1);
SQL

    "$TMP_PGO/pgo_train" "$TMP_PGO/scratch.db" 2>/dev/null || true
    $PROFDATA_CMD merge -output="$TMP_PGO/sqlite.profdata" "$TMP_PGO/prof"/*.profraw 2>/dev/null || true

    if [ -f "$TMP_PGO/sqlite.profdata" ]; then
      if clang -c src/shim.c \
        -I deps/$SQLITE \
        -O3 -march=native -fno-stack-protector -fomit-frame-pointer -fno-semantic-interposition \
        -fprofile-use="$TMP_PGO/sqlite.profdata" \
        -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
        -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE \
        -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
        -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1 \
        -DSQLITE_DISABLE_DIRSYNC -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_EXPLAIN \
        -o src/c/shim_pgo.o 2>/dev/null; then
        PGO_FLAG="-Dpgo=true"
        echo "== Compiled PGO SQLite shim successfully =="
      fi
    fi
  fi
  rm -rf "$TMP_PGO"
fi

if [ -z "$PGO_FLAG" ]; then
  echo "Notice: Clang PGO build skipped; falling back to native Zig build"
  rm -f src/c/shim_pgo.o
fi

zig build -Doptimize=ReleaseFast -Dcpu=native $PGO_FLAG
