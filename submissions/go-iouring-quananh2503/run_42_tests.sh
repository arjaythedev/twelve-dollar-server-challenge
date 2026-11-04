#!/usr/bin/env bash
set -e

pkill -9 -f "go-iouring-quananh2503/bin/server" 2>/dev/null || true
fuser -k 3000/tcp 6060/tcp 2>/dev/null || true
sleep 1

rm -f /tmp/test_feed.db /tmp/test_feed.db-wal /tmp/test_feed.db-shm
cp seed/feed.db /tmp/test_feed.db

export SQLITE_PATH=/tmp/test_feed.db
export PORT=3000
export HOST=127.0.0.1
export JWT_SECRET=twelve-dollar-challenge

./submissions/go-iouring-quananh2503/bin/server > /tmp/server_test.log 2>&1 &
PID=$!

cleanup() {
    kill -9 "$PID" 2>/dev/null || true
    pkill -9 -f "go-iouring-quananh2503/bin/server" 2>/dev/null || true
    rm -f /tmp/test_feed.db /tmp/test_feed.db-wal /tmp/test_feed.db-shm
}
trap cleanup EXIT

for i in {1..30}; do
    if curl -s http://127.0.0.1:3000/health | grep -q "status"; then
        echo "Server is ready on port 3000"
        break
    fi
    sleep 0.2
done

bash test/test.sh http://127.0.0.1:3000
