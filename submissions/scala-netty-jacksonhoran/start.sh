#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
# Serial GC: one vCPU, so a parallel or concurrent collector only adds threads competing with the
# event loop. Long-lived connection objects are promoted after 2 young GCs so they stop being copied.
exec /opt/jdk-25/bin/java \
  -XX:+UseSerialGC -Xms512m -Xmx512m -Xmn192m -XX:MaxTenuringThreshold=2 -XX:+AlwaysPreTouch \
  -XX:MaxDirectMemorySize=256m -XX:-UsePerfData \
  --enable-native-access=ALL-UNNAMED --sun-misc-unsafe-memory-access=allow \
  -Dio.netty.leakDetection.level=disabled \
  -Dsqlite.lib="$PWD/native/libsqlite3.so" \
  -cp "$(cat target/classpath.txt)" server.Main
