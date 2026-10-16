#!/usr/bin/env bash
# start.sh: Runs the server in the foreground, configured only by SPEC.md env vars.
set -euo pipefail

cd "$(dirname "$0")"
JAVA_BIN="java"
if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/java" ]; then
  JAVA_BIN="$JAVA_HOME/bin/java"
elif [ -x "/opt/homebrew/opt/openjdk/bin/java" ]; then
  JAVA_BIN="/opt/homebrew/opt/openjdk/bin/java"
elif command -v /usr/libexec/java_home >/dev/null 2>&1; then
  JH="$(/usr/libexec/java_home -v 21 2>/dev/null || /usr/libexec/java_home 2>/dev/null || true)"
  if [ -n "$JH" ] && [ -x "$JH/bin/java" ]; then
    JAVA_BIN="$JH/bin/java"
  fi
fi

exec "$JAVA_BIN" \
  -XX:+UseSerialGC \
  -Xms512m \
  -Xmx512m \
  -Xmn384m \
  -XX:+AlwaysPreTouch \
  -XX:CompileThreshold=1500 \
  -Dio.netty.allocator.type=pooled \
  -Dio.netty.allocator.numDirectArenas=1 \
  -Dio.netty.allocator.numHeapArenas=0 \
  -Dio.netty.buffer.checkBounds=false \
  -Dio.netty.buffer.checkAccessible=false \
  -jar target/server.jar
