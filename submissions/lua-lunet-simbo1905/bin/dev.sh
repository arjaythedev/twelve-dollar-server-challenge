#!/usr/bin/env bash
# Dev-only runner: pid-file managed start/stop. test/run.sh uses start.sh directly.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
PID_FILE="$ROOT/target/lunet.pid"
LOG_FILE="$ROOT/target/server.log"
SEED_DB="$ROOT/../../seed/feed.db"
SQLITE_PATH="${SQLITE_PATH:-$ROOT/target/feed.db}"
export HOST="${HOST:-127.0.0.1}" PORT="${PORT:-3000}" JWT_SECRET="${JWT_SECRET:-twelve-dollar-challenge}" SQLITE_PATH
mkdir -p "$ROOT/target"

live_pid() {
    local pid
    pid="$(cat "$PID_FILE" 2>/dev/null)"
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null || { rm -f "$PID_FILE"; return 1; }
    echo "$pid"
}

cmd_start() {
    local pid i healthy=""
    if pid="$(live_pid)"; then echo "already running (pid $pid)"; return 0; fi
    if [ "$SQLITE_PATH" = "$ROOT/target/feed.db" ]; then rm -f "$SQLITE_PATH" "$SQLITE_PATH-shm" "$SQLITE_PATH-wal"; cp "$SEED_DB" "$SQLITE_PATH"; fi
    : > "$LOG_FILE"
    bash start.sh >>"$LOG_FILE" 2>&1 &
    pid=$!; echo "$pid" > "$PID_FILE"
    sleep 2
    if ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$PID_FILE"; echo "server died at startup (pid $pid), log tail:"; tail -20 "$LOG_FILE"; return 1
    fi
    for i in 1 2 3 4 5 6 7 8; do
        curl -sf -m 2 "http://$HOST:$PORT/health" >/dev/null 2>&1 && { healthy=1; break; }
        sleep 1
    done
    sleep 1
    if [ -z "$healthy" ] || ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$PID_FILE"; kill "$pid" 2>/dev/null; echo "server not healthy (pid $pid), log tail:"; tail -20 "$LOG_FILE"; return 1
    fi
    echo "running (pid $pid) on http://$HOST:$PORT — log: $LOG_FILE, db: $SQLITE_PATH"
}

cmd_stop() {
    local pid i
    if ! pid="$(live_pid)"; then echo "not running"; return 0; fi
    kill "$pid" 2>/dev/null
    for i in 1 2 3 4 5 6 7 8; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$pid" 2>/dev/null; then echo "pid $pid would not die — pid file left: $PID_FILE"; return 1; fi
    rm -f "$PID_FILE"
    echo "stopped (pid $pid)"
}

case "${1:-}" in
    start)  cmd_start ;;
    stop)   cmd_stop ;;
    status) if pid="$(live_pid)"; then echo "running (pid $pid) on http://$HOST:$PORT"; else echo "not running"; fi ;;
    *)      echo "usage: $0 start|stop|status"; exit 2 ;;
esac
