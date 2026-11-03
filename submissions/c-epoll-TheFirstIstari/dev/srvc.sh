#!/usr/bin/env bash
# srvc.sh start|stop|check <container> [port] [binary]
# Bulletproof server lifecycle: kills EVERYTHING, verifies by uptime + solo listener.
# Usage (on cachyos): bash dev/srvc.sh start s12 3100 /sub/bin/server
set -u
CMD=${1:?start|stop|check}; C=${2:?container}; PORT=${3:-3100}; BIN=${4:-/sub/bin/server}
NAMES="server feed-pace2 feed-guard feed-ckpt feed-cnt feed-final feed-opt feed-dbg feed-t1 feed-t2 feed12 twelve-rust-poll"
killall() {
  for n in $NAMES; do docker exec "$C" pkill -x "$n" 2>/dev/null; done
  docker exec "$C" bash -c "pkill -f start.s[h] 2>/dev/null; true"
  sleep 2
}
srvlive() {
  docker exec "$C" bash -c "ps -eo comm,stat,args" | grep -E "bin/server|feed-" | grep -v defunct | grep -v grep | wc -l
}
if [ "$CMD" = stop ] || [ "$CMD" = restart ]; then
  killall
fi
if [ "$CMD" = start ] || [ "$CMD" = restart ]; then
  killall
  docker exec "$C" bash -c "cp /repo/seed/feed.db /tmp/srv.db && rm -f /tmp/srv.db-wal /tmp/srv.db-shm" || exit 1
  # shellcheck disable=SC2086
  docker exec -d "$C" bash -c "exec env SQLITE_PATH=/tmp/srv.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=$PORT $BIN > /tmp/srv.log 2>&1"
  sleep 2
fi
N=$(srvlive)
H=$(curl -s -m 5 "http://127.0.0.1:$PORT/health" || echo NONE)
U=$(echo "$H" | grep -oE '"uptime_s":[0-9]+' | grep -oE '[0-9]+')
[ -z "$U" ] && U=999999
echo "live=$N health_uptime=$U"
if [ "$CMD" = stop ]; then
  [ "$N" = 0 ] && echo STOP-OK || { echo "STOP FAILED"; exit 1; }
  exit 0
fi
if [ "$N" != 1 ] || [ "$U" -ge 60 ]; then
  echo "CHECK FAILED"
  exit 1
fi
echo "$H" | grep -q '"status":"ok"' && echo CHECK-OK || { echo "CHECK FAILED"; exit 1; }
