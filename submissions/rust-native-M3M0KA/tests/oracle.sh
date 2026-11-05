#!/usr/bin/env bash
# Differential test: the B-tree read plans must produce byte-identical responses
# to the ordinary SQL statements.
#
#   bash tests/oracle.sh [number-of-random-posts]
#
# Two servers read the same database state: one with the B-tree read plans, one
# with FEED_NO_PLAN=1 (pure SQL). Stage 1 compares a fresh seed; stage 2 applies
# writes through the plan server, then both servers are restarted on copies of
# that exact database and compared again.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
N=${1:-3000}
PORT_A=${PORT_A:-3211}
PORT_B=${PORT_B:-3212}
WORK=$(mktemp -d)
TOKEN=$(jq -r '.[0].token' "$ROOT/seed/tokens.json")
PID_A=""
PID_B=""

cleanup() {
  for p in $PID_A $PID_B; do
    [ -n "${p:-}" ] || continue
    # Workers are children: kill them before the parent, or they are
    # reparented to init and keep the port.
    pkill -P "$p" 2>/dev/null || true
    kill "$p" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

start() { # port, extra env, db -> pid in $STARTED
  SQLITE_PATH="$3" JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT="$1" \
    env $2 "$HERE/bin/feed" >"$WORK/server-$1.log" 2>&1 &
  STARTED=$!
}

wait_up() {
  for _ in $(seq 300); do curl -sf "http://127.0.0.1:$1/health" >/dev/null && return 0; sleep 0.1; done
  echo "server on $1 never became healthy"; cat "$WORK/server-$1.log"; exit 1
}

stop() {
  pkill -P "$1" 2>/dev/null || true
  kill "$1" 2>/dev/null || true
  wait "$1" 2>/dev/null || true
}

grab() { # port, outdir
  mkdir -p "$2"
  curl -s "http://127.0.0.1:$1/feed" > "$2/feed.json"
  while read -r id; do
    printf '%s ' "$id"
    curl -s "http://127.0.0.1:$1/posts/$id" | md5sum | cut -d' ' -f1
  done < "$WORK/ids.txt" > "$2/posts.txt"
  : > "$2/edges.txt"
  for id in 1 2 3 499998 499999 500000 500001 500002 999999999; do
    curl -s "http://127.0.0.1:$1/posts/$id" >> "$2/edges.txt"
    printf '\n' >> "$2/edges.txt"
  done
}

shuf -i 1-500000 -n "$N" | sort -n > "$WORK/ids.txt"
cp "$ROOT/seed/feed.db" "$WORK/a.db"
cp "$ROOT/seed/feed.db" "$WORK/b.db"

fail=0
compare() { # label
  if ! cmp -s "$WORK/plan/feed.json" "$WORK/sql/feed.json"; then
    echo "FAIL ($1): /feed differs"; fail=1
  fi
  if ! cmp -s "$WORK/plan/posts.txt" "$WORK/sql/posts.txt"; then
    echo "FAIL ($1): /posts/:id differs"
    diff "$WORK/plan/posts.txt" "$WORK/sql/posts.txt" | head -5
    fail=1
  fi
  if ! cmp -s "$WORK/plan/edges.txt" "$WORK/sql/edges.txt"; then
    echo "FAIL ($1): edge ids differ"; fail=1
  fi
}

start "$PORT_A" "" "$WORK/a.db"; PID_A=$STARTED
wait_up "$PORT_A"
grep -q "read plans: enabled" "$WORK/server-$PORT_A.log" || {
  echo "the plan did not activate; this test would prove nothing"
  cat "$WORK/server-$PORT_A.log"; exit 1; }

start "$PORT_B" "FEED_NO_PLAN=1" "$WORK/b.db"; PID_B=$STARTED
wait_up "$PORT_B"
grep -q "disabled by FEED_NO_PLAN" "$WORK/server-$PORT_B.log" || {
  echo "the SQL reference did not disable the plan"; exit 1; }

grab "$PORT_A" "$WORK/plan"; grab "$PORT_B" "$WORK/sql"
compare "fresh seed"

# Writes through the plan server, then both servers see the same file.
for _ in 1 2 3; do
  curl -s -X POST "http://127.0.0.1:$PORT_A/posts" \
    -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
    -d '{"body":"oracle probe post"}' > /dev/null
done
curl -s -X POST "http://127.0.0.1:$PORT_A/posts/1/like" -H "Authorization: Bearer $TOKEN" > /dev/null
curl -s -X POST "http://127.0.0.1:$PORT_A/posts/250000/like" -H "Authorization: Bearer $TOKEN" > /dev/null
curl -s -X POST "http://127.0.0.1:$PORT_A/posts/500001/like" -H "Authorization: Bearer $TOKEN" > /dev/null

stop "$PID_A"; stop "$PID_B"
# Make the copy self-contained: checkpoint the WAL, then copy the file.
sqlite3 "$WORK/a.db" "PRAGMA wal_checkpoint(TRUNCATE);" > /dev/null
rm -f "$WORK/b.db" "$WORK/b.db-wal" "$WORK/b.db-shm"
cp "$WORK/a.db" "$WORK/b.db"
cp "$WORK/a.db" "$WORK/a2.db"

start "$PORT_A" "" "$WORK/a2.db"; PID_A=$STARTED
wait_up "$PORT_A"
start "$PORT_B" "FEED_NO_PLAN=1" "$WORK/b.db"; PID_B=$STARTED
wait_up "$PORT_B"
rm -rf "$WORK/plan" "$WORK/sql"
grab "$PORT_A" "$WORK/plan"; grab "$PORT_B" "$WORK/sql"
compare "after writes"

# Stage 3: writes this server did not make. A backdated timestamp, a rowid gap
# and two posts with the same timestamp are exactly the cases a plan that
# assumed dense ordered ids would get wrong or switch itself off over.
stop "$PID_A"; stop "$PID_B"
sqlite3 "$WORK/a2.db" "UPDATE posts SET created_at='1999-01-01T00:00:00.000Z' WHERE id = 499990;
  INSERT INTO posts (id, user_id, body, created_at) VALUES (700000, 1, 'rowid gap', '2026-12-31T23:59:59.999Z');
  INSERT INTO posts (user_id, body, created_at) VALUES (2, 'same timestamp', '2026-12-31T23:59:59.999Z');" >/dev/null
sqlite3 "$WORK/a2.db" "PRAGMA wal_checkpoint(TRUNCATE);" >/dev/null
rm -f "$WORK/b.db" "$WORK/b.db-wal" "$WORK/b.db-shm"
cp "$WORK/a2.db" "$WORK/b.db"
start "$PORT_A" "" "$WORK/a2.db"; PID_A=$STARTED
wait_up "$PORT_A"
start "$PORT_B" "FEED_NO_PLAN=1" "$WORK/b.db"; PID_B=$STARTED
wait_up "$PORT_B"
rm -rf "$WORK/plan" "$WORK/sql"
grab "$PORT_A" "$WORK/plan"; grab "$PORT_B" "$WORK/sql"
compare "after foreign writes"
if grep -q "read plans disabled" "$WORK/server-$PORT_A.log"; then
  echo "FAIL: the plan disabled itself after foreign writes"; fail=1
fi

echo "compared /feed and $N random posts + 8 edge ids, before and after writes, and after foreign writes"
[ "$fail" = 0 ] && echo "oracle: identical" || { echo "oracle: MISMATCH"; exit 1; }
