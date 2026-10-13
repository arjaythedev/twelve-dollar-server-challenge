#!/usr/bin/env bash
# Checks the server at its descriptor limit: no busy loop while full, and it accepts again once
# connections close. Run on Linux from the repo root after build.sh:
#
#   bash submissions/go-epoll-damianoneill/tests/fdlimit.sh submissions/go-epoll-damianoneill/bin/server
set -euo pipefail
BIN="$1" PORT=3002
DB="$(mktemp -d)/feed.db"
cp seed/feed.db "$DB"
(ulimit -n 64; SQLITE_PATH="$DB" JWT_SECRET=x HOST=127.0.0.1 PORT=$PORT exec "$BIN") &
pid=$!
trap 'kill $pid 2>/dev/null; rm -rf "$(dirname "$DB")"' EXIT
until (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; do sleep 0.2; done

fds=()
for _ in $(seq 100); do exec {fd}<>/dev/tcp/127.0.0.1/$PORT; fds+=("$fd"); done
sleep 1
ticks() { awk '{ print $14 + $15 }' /proc/$pid/stat; }
t0=$(ticks); sleep 2; t1=$(ticks)
busy=$(( (t1 - t0) / 2 ))
echo "CPU while full: ${busy}%"

for fd in "${fds[@]}"; do eval "exec $fd>&-"; done
health=$(curl -s -m 5 "http://127.0.0.1:$PORT/health" || true)
echo "after closing: $health"
[ "$busy" -lt 10 ] && [[ "$health" == *'"status":"ok"'* ]]
