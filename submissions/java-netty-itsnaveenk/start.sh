#!/usr/bin/env bash
# start.sh: Runs the server in the foreground, configured only by SPEC.md env vars.
set -euo pipefail

cd "$(dirname "$0")"
JAVA_BIN="java"
for jdir in \
  /usr/lib/jvm/java-21-openjdk-amd64 \
  /usr/lib/jvm/java-21-openjdk-arm64 \
  "${JAVA_HOME_21_X64:-}" \
  "${JAVA_HOME_21_ARM64:-}" \
  /opt/homebrew/opt/openjdk@21 \
  /opt/homebrew/opt/openjdk
do
  if [ -n "$jdir" ] && [ -x "$jdir/bin/java" ]; then
    JAVA_BIN="$jdir/bin/java"
    break
  fi
done

if [ "$JAVA_BIN" = "java" ]; then
  if command -v /usr/libexec/java_home >/dev/null 2>&1; then
    JH="$(/usr/libexec/java_home -v 21 2>/dev/null || /usr/libexec/java_home 2>/dev/null || true)"
    if [ -n "$JH" ] && [ -x "$JH/bin/java" ]; then
      JAVA_BIN="$JH/bin/java"
    fi
  elif [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/java" ]; then
    JAVA_BIN="$JAVA_HOME/bin/java"
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
