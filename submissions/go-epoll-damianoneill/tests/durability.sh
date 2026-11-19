#!/usr/bin/env bash
# Checks that every acknowledged write survives SIGKILL straight after its response arrives. The
# writes are pipelined on one connection so they share batches. Run on Linux from the repo root after
# build.sh:
#
#   bash submissions/go-epoll-damianoneill/tests/durability.sh submissions/go-epoll-damianoneill/bin/server
set -euo pipefail
BIN="$1" PORT=3003
DIR="$(mktemp -d)"
cp seed/feed.db "$DIR/feed.db"
SQLITE_PATH="$DIR/feed.db" JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=$PORT "$BIN" &
pid=$!
trap 'kill -9 $pid 2>/dev/null || true; rm -rf "$DIR"' EXIT
until (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; do sleep 0.2; done

token=$(jq -r '.[1].token' seed/tokens.json) # user 2
exec 3<>/dev/tcp/127.0.0.1/$PORT
for i in $(seq 50); do
  body="{\"body\":\"durability $i\"}"
  printf 'POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: %d\r\n\r\n%s' "$token" ${#body} "$body"
  printf 'POST /posts/%d/like HTTP/1.1\r\nAuthorization: Bearer %s\r\n\r\n' $((1000 + i)) "$token"
done >&3
printf 'GET /health HTTP/1.1\r\nConnection: close\r\n\r\n' >&3
replies=$(timeout 10 cat <&3)
kill -9 $pid

acked=$(grep -o 'HTTP/1.1 20[01]' <<<"$replies" | wc -l)
posts=$(sqlite3 "$DIR/feed.db" "SELECT count(*) FROM posts WHERE user_id = 2 AND body LIKE 'durability %'")
likes=$(sqlite3 "$DIR/feed.db" "SELECT count(*) FROM likes WHERE user_id = 2 AND post_id BETWEEN 1001 AND 1050")
echo "acknowledged=$acked (want 101) posts=$posts likes=$likes (want 50 each)"
[ "$acked" -eq 101 ] && [ "$posts" -eq 50 ] && [ "$likes" -eq 50 ]
