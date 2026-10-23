#!/usr/bin/env bash
# Runs the server in the foreground, configured only by SQLITE_PATH, JWT_SECRET, HOST and PORT.
set -euo pipefail
cd "$(dirname "$0")"
DB_DIR="$(dirname "$SQLITE_PATH")"
# In-process settings (rule 11): every fd the hard limit allows, one per connection.
ulimit -Sn "$(ulimit -Hn)" 2>/dev/null || true
# If the box runs out of memory, kill us first rather than sshd or journald: our sockets free with us.
{ echo 1000 > /proc/self/oom_score_adj; } 2>/dev/null || true
# Restart after a crash or the OOM killer (SIGKILL), so one failed probe does not fail every later run.
# A normal exit, a startup error, SIGTERM or SIGINT end the script.
while :; do
  s=0
  # --single-threaded-gc: no V8 GC helper threads competing with the server for the one vCPU
  # (droplet, 40K VUs: p99 1,122 → 750 ms, p95 127 → 63 ms at the same CPU).
  deno run \
    --allow-env=SQLITE_PATH,JWT_SECRET,HOST,PORT \
    --allow-net \
    --allow-read="$DB_DIR" --allow-write="$DB_DIR" \
    --allow-ffi \
    --v8-flags=--single-threaded-gc \
    server.ts || s=$?
  case $s in 130 | 143) exit $s ;; esac
  [ $s -ge 128 ] || exit $s
  echo "server died with status $s, restarting" >&2
  sleep 1
done
