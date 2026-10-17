#!/usr/bin/env bash
# Checks that the WAL is checkpointed: thousands of likes over many batches leave the WAL file near
# SQLite's 1,000 page threshold instead of growing with every write. Run on Linux from the repo root
# after build.sh:
#
#   bash submissions/go-epoll-damianoneill/tests/walsize.sh submissions/go-epoll-damianoneill/bin/server
set -euo pipefail
BIN="$1" PORT=3005 LIKES=4000
DIR="$(mktemp -d)"
cp seed/feed.db "$DIR/feed.db"
SQLITE_PATH="$DIR/feed.db" JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=$PORT "$BIN" &
pid=$!
trap 'kill $pid 2>/dev/null || true; rm -rf "$DIR"' EXIT
until (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; do sleep 0.2; done

token=$(jq -r '.[2].token' seed/tokens.json)
exec 3<>/dev/tcp/127.0.0.1/$PORT
{
  for i in $(seq $LIKES); do
    printf 'POST /posts/%d/like HTTP/1.1\r\nAuthorization: Bearer %s\r\n\r\n' $(( i * 7919 % 500000 + 1 )) "$token"
  done
  printf 'GET /health HTTP/1.1\r\nConnection: close\r\n\r\n'
} >&3 &
ok=$(timeout 60 cat <&3 | grep -ac 'HTTP/1.1 20[01]')
wal=$(stat -c %s "$DIR/feed.db-wal")
echo "acknowledged=$ok (want $(( LIKES + 1 ))) wal=$(( wal / 1048576 )) MiB (want under 16)"
[ "$ok" -eq $(( LIKES + 1 )) ] && [ "$wal" -lt $(( 16 << 20 )) ]
