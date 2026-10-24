#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
JAVA_HOME="${JAVA_HOME:-/opt/jdk-25}"
# Serial GC: one vCPU, so a parallel or concurrent collector only adds threads competing with the app.
# A fixed, pre-touched heap, mostly young generation: behind Nginx almost nothing lives longer than a request.
# Fiber tracing off: it keeps a ring buffer of events in every fiber.
exec "$JAVA_HOME/bin/java" \
  -XX:+UseSerialGC -Xms512m -Xmx512m -Xmn320m -XX:+AlwaysPreTouch -Xss512k \
  -XX:MaxDirectMemorySize=128m -XX:-UsePerfData -Dcats.effect.tracing.mode=none \
  --enable-native-access=ALL-UNNAMED --sun-misc-unsafe-memory-access=allow \
  -cp "$(cat target/classpath.txt)" feed.Main
