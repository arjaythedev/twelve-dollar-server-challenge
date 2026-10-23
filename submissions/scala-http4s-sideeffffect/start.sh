#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
JAVA_HOME="${JAVA_HOME:-/opt/jdk-25}"
# Serial GC: one vCPU, so a parallel or concurrent collector only adds threads competing with the app.
# A fixed, pre-touched heap: 2 GB box, and the app shouldn't grow into the OS's share.
exec "$JAVA_HOME/bin/java" \
  -XX:+UseSerialGC -Xms640m -Xmx640m -Xmn256m -XX:+AlwaysPreTouch -Xss512k \
  -XX:MaxDirectMemorySize=128m -XX:-UsePerfData \
  --enable-native-access=ALL-UNNAMED \
  -cp "$(cat target/classpath.txt)" feed.Main
