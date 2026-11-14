#!/usr/bin/env bash
# Checks that a write whose commit fails is never acknowledged, and that an acknowledged write is in
# the database. A deferred foreign key on a small database makes a like from a missing user fail at
# COMMIT, pipelined behind a valid post. Run on Linux from the repo root after build.sh:
#
#   bash submissions/go-epoll-damianoneill/tests/commitfail.sh
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$(mktemp -d)" PORT=3004 SECRET=commit-test
trap 'kill $pid 2>/dev/null || true; rm -rf "$DIR"' EXIT

(cd "$SRC" && PATH=/usr/local/go/bin:$PATH CGO_ENABLED=1 go build -o "$DIR/server" \
  -ldflags "-X 'main.pragmas=PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;'" .)
sed 's/REFERENCES users(id),/REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,/' schema.sql | sqlite3 "$DIR/feed.db"
sqlite3 "$DIR/feed.db" "PRAGMA journal_mode=WAL; INSERT INTO users (id, username) VALUES (1, 'one');
  INSERT INTO posts (id, user_id, body) VALUES (1, 1, 'first');" >/dev/null

SQLITE_PATH="$DIR/feed.db" JWT_SECRET=$SECRET HOST=127.0.0.1 PORT=$PORT "$DIR/server" 2>/dev/null &
pid=$!
until (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; do sleep 0.2; done

b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }
sign() {
  local h p
  h=$(printf '%s' '{"alg":"HS256","typ":"JWT"}' | b64url); p=$(printf '%s' "$1" | b64url)
  echo "$h.$p.$(printf '%s' "$h.$p" | openssl dgst -sha256 -hmac "$SECRET" -binary | b64url)"
}
exp=$(( $(date +%s) + 600 ))
one=$(sign "{\"sub\":\"1\",\"username\":\"one\",\"exp\":$exp}")
ghost=$(sign "{\"sub\":\"999\",\"username\":\"ghost\",\"exp\":$exp}")

body='{"body":"commit test"}'
exec 3<>/dev/tcp/127.0.0.1/$PORT
printf 'POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n%s' "$one" ${#body} "$body" >&3
printf 'POST /posts/1/like HTTP/1.1\r\nAuthorization: Bearer %s\r\n\r\n' "$ghost" >&3
printf 'GET /health HTTP/1.1\r\nConnection: close\r\n\r\n' >&3
statuses=($(timeout 5 cat <&3 | grep -ao 'HTTP/1.1 [0-9]*' | cut -d' ' -f2))
kill $pid; wait $pid 2>/dev/null || true

post_rows=$(sqlite3 "$DIR/feed.db" "SELECT count(*) FROM posts WHERE body = 'commit test'")
like_rows=$(sqlite3 "$DIR/feed.db" "SELECT count(*) FROM likes WHERE user_id = 999")
echo "statuses=${statuses[*]} post_rows=$post_rows like_rows=$like_rows"
post_acked=$([ "${statuses[0]:-}" = 201 ] && echo 1 || echo 0)
[[ "${statuses[1]:-}" != 20? ]] && [ "$like_rows" -eq 0 ] && [ "$post_rows" -eq "$post_acked" ]
