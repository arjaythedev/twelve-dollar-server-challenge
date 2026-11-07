#!/usr/bin/env bash
# Training load for the profile-guided build. build.sh calls it with the instrumented bin/server in place.
#
# It makes a scratch database from train/schema.sql (a copy of the challenge's schema.sql, same tables and indexes,
# filled with generated rows), starts the instrumented server on 127.0.0.1:$TRAIN_PORT, sends the mix of
# bench/load.js (GET /feed, GET /posts/:id, likes, creates) with curl, then stops the server with SIGTERM, which
# makes it write pgo/*.gcda. Exit 0 only when profile files exist. Tools: bash, curl, openssl, sqlite3 (install.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${TRAIN_PORT:-18080}"
SECRET=pgo-training-secret
RUN=pgo/run
rm -rf "$RUN" pgo/*.gcda pgo/opt 2> /dev/null || true
mkdir -p "$RUN"

sqlite3 "$RUN/train.db" < train/schema.sql
sqlite3 "$RUN/train.db" << 'SQL'
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2000)
INSERT INTO users (id, username) SELECT i, 'user_' || i FROM n;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 40000)
INSERT INTO posts (user_id, body, created_at)
  SELECT (i * 7919) % 2000 + 1, 'training post ' || i || ' ' || hex(randomblob(40)),
         strftime('%Y-%m-%dT%H:%M:%fZ', '2026-01-01', '+' || i || ' seconds') FROM n;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 120000)
INSERT OR IGNORE INTO likes (user_id, post_id) SELECT (i * 104729) % 2000 + 1, (i * 15485863) % 40000 + 1 FROM n;
SQL

b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }
token() { # token <user id>: HS256 JWT in the shape of the seed tokens
  local h p s
  h=$(printf '{"alg":"HS256","typ":"JWT"}' | b64url)
  p=$(printf '{"sub":"%s","username":"user_%s","iat":1700000000,"exp":4000000000}' "$1" "$1" | b64url)
  s=$(printf '%s.%s' "$h" "$p" | openssl dgst -sha256 -hmac "$SECRET" -binary | b64url)
  printf '%s.%s.%s' "$h" "$p" "$s"
}

JWT_SECRET="$SECRET" HOST=127.0.0.1 PORT="$PORT" SQLITE_PATH="$RUN/train.db" ./bin/server > "$RUN/server.log" 2>&1 &
SP=$!
trap 'kill -KILL "$SP" 2> /dev/null || true' EXIT
for _ in $(seq 100); do
  curl -sf "http://127.0.0.1:$PORT/health" > /dev/null && break
  sleep 0.2
done
curl -sf "http://127.0.0.1:$PORT/health" > /dev/null || { echo "train: server did not start"; exit 1; }

U="http://127.0.0.1:$PORT"
pids=()
# reads: about 85% of the requests, each worker on its own connection
for k in 0 1 2 3 4 5 6 7; do
  curl -s -o /dev/null "$U/feed?[1-4000]" & pids+=($!)
  curl -s -o /dev/null "$U/posts/[$((k * 4500 + 1))-$((k * 4500 + 4000))]" & pids+=($!)
done
# likes (some repeat a pair, which exercises the conflict path) and creates, as users 1 to 4
for k in 1 2 3 4; do
  T=$(token "$k")
  curl -s -o /dev/null -X POST -H "Authorization: Bearer $T" "$U/posts/[$((k * 700))-$((k * 700 + 899))]/like" & pids+=($!)
  curl -s -o /dev/null -X POST -H "Authorization: Bearer $T" -H 'Content-Type: application/json' \
    -d "{\"body\":\"training post from user $k\"}" "$U/posts?[1-150]" & pids+=($!)
done
for p in "${pids[@]}"; do wait "$p" || true; done

kill -TERM "$SP"
for _ in $(seq 100); do kill -0 "$SP" 2> /dev/null || break; sleep 0.1; done
trap - EXIT
n=$(find pgo -name '*.gcda' | wc -l)
[ "$n" -ge 4 ] || { echo "train: only $n profile files written"; exit 1; }
echo "train: $n profile files"
