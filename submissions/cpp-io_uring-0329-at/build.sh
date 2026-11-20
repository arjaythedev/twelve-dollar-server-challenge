#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] && export XMAKE_ROOT=y

SQ=sqlite-amalgamation-3540100
SQ_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
if [ ! -f sqlite3.c ] && [ ! -f vendor/sqlite3.c ]; then
  mkdir -p vendor
  if [ ! -f vendor/$SQ.zip ]; then
    curl -fsSL --retry 3 -o vendor/$SQ.zip "https://www.sqlite.org/2026/$SQ.zip"
  fi
  echo "$SQ_SHA  vendor/$SQ.zip" | sha256sum -c -
  (cd vendor && unzip -qo $SQ.zip && mv $SQ/sqlite3.c $SQ/sqlite3.h . && rm -rf $SQ $SQ.zip)
fi

PGO="${PGO:-1}"
PGO_DIR="/tmp/pgo-$$"

# 训练：起一个临时实例，跑完整的 42 项检查（覆盖所有 handler），再补一段 /feed 的热路径。
# 只用 shell + curl，CI 上没有 k6 也能跑。任何一步失败都退回普通构建。
train_pgo() {
  local db port=3987 log
  db="$(mktemp -d)/feed.db"
  log="$(dirname "$db")/server.log"
  cp ../../seed/feed.db "$db"
  rm -f "$db"-wal "$db"-shm
  SQLITE_PATH="$db" JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=$port \
    ./server > "$log" 2>&1 &
  local pid=$!
  for _ in $(seq 50); do
    curl -s -m 1 -o /dev/null "http://127.0.0.1:$port/health" && break
    sleep 0.1
  done
  if ! curl -s -m 1 -o /dev/null "http://127.0.0.1:$port/health"; then
    kill "$pid" 2>/dev/null; echo "pgo: server did not start" >&2; return 1
  fi
  local tok scripts
  scripts="$(cd "$(dirname "$0")/../.." && pwd)/test/test.sh"
  [ -f "$scripts" ] && bash "$scripts" "http://127.0.0.1:$port" > /dev/null 2>&1
  tok=$(python3 - <<'PY' 2>/dev/null || true
import json,sys
try:
    d=json.load(open('../../seed/tokens.json'))
    print(d[0]['token'])
except Exception:
    pass
PY
)
  for _ in $(seq 300); do
    if [ -n "${tok:-}" ]; then
      curl -s -m 2 -o /dev/null -H "Authorization: Bearer $tok" "http://127.0.0.1:$port/feed"
    else
      curl -s -m 2 -o /dev/null "http://127.0.0.1:$port/feed"
    fi
  done
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null || true
  return 0
}

if [ "$PGO" = "1" ]; then
  rm -rf "$PGO_DIR"; mkdir -p "$PGO_DIR"
  echo "== pgo pass 1 (generate)"
  PGO_MODE=generate PGO_DIR="$PGO_DIR" xmake f -c -m release -y
  PGO_MODE=generate PGO_DIR="$PGO_DIR" xmake -j"$(nproc)" -y
  echo "== pgo training"
  if train_pgo; then
    echo "== pgo pass 2 (use)"
    PGO_MODE=use PGO_DIR="$PGO_DIR" xmake f -c -m release -y
    PGO_MODE=use PGO_DIR="$PGO_DIR" xmake -j"$(nproc)" -y
  else
    echo "== pgo training failed, falling back to a plain build" >&2
    xmake f -c -m release -y
    xmake -j"$(nproc)" -y
  fi
else
  xmake f -c -m release -y
  xmake -j"$(nproc)" -y
fi
[ -x server ] || { echo "build failed: no ./server" >&2; exit 1; }
ls -l server
