#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT (+ optional WORKERS, default 2).
# WORKERS processes share the port via SO_REUSEPORT. RLIMIT_NOFILE is per process, so 2 workers can hold
# ~130k connections instead of the 65,535 that one process is capped at.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
JAVA=java
[ -x /opt/jdk-25/bin/java ] && JAVA=/opt/jdk-25/bin/java
WORKERS="${WORKERS:-2}"

pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true' EXIT INT TERM
for i in $(seq 0 $((WORKERS - 1))); do
  WORKER_ID=$i "$JAVA" \
    -XX:+UseSerialGC -Xms128m -Xmx320m -Xss512k \
    -XX:+UseCompactObjectHeaders -XX:-UsePerfData \
    --enable-native-access=ALL-UNNAMED \
    -cp "$DIR/build/lib/*" nio.MainKt &
  pids+=($!)
done
wait -n   # if any worker dies, stop them all
exit 1
