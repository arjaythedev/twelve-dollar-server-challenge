#!/usr/bin/env bash
# End-to-end tests: the challenge's own conformance suite, then the protocol tests.
#
#   SERVER_BIN=bin/server SEED_DB=<feed.db> CHALLENGE_ROOT=<challenge repo> bash tests/e2e/run.sh
#
# Needs curl, jq, openssl and python3 on PATH. Each part starts its own server on a fresh database copy.
set -euo pipefail
: "${SERVER_BIN:?path to the server binary}" "${SEED_DB:?path to seed/feed.db}" "${CHALLENGE_ROOT:?path to the challenge repository}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SERVER_BIN="$(realpath "$SERVER_BIN")"
WORK="$(mktemp -d)"
SERVER_PID=""
stop_server() {
  # Never `kill 0`: PID 0 means the whole process group.
  if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; SERVER_PID=""; fi
}
trap 'stop_server; rm -rf "$WORK"' EXIT

export JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT="${PORT:-3000}" SQLITE_PATH="$WORK/feed.db"
cp "$SEED_DB" "$SQLITE_PATH"; chmod u+w "$SQLITE_PATH"

# Everything runs once per I/O backend. SERVER_IO forces one (the server picks io_uring when the kernel allows it).
for backend in ${IO_BACKENDS:-epoll uring}; do
  export SERVER_IO=$backend
  echo "== [$backend] conformance (challenge test/test.sh)"
  rm -f "$WORK"/feed.db*; cp "$SEED_DB" "$SQLITE_PATH"; chmod u+w "$SQLITE_PATH"
  "$SERVER_BIN" > "$WORK/server.log" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 100); do curl -sf "http://$HOST:$PORT/health" >/dev/null && break; sleep 0.2; done
  bash "$CHALLENGE_ROOT/test/test.sh" "http://$HOST:$PORT"
  stop_server

  echo "== [$backend] protocol tests"
  SERVER_BIN="$SERVER_BIN" SEED_DB="$SEED_DB" python3 -I "$HERE/protocol_test.py"
done
