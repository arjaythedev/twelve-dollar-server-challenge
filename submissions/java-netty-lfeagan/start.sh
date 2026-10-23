#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
exec /opt/jdk-25/bin/java \
  -XX:+UseSerialGC -Xms512m -Xmx512m -XX:+AlwaysPreTouch \
  -XX:+UseCompactObjectHeaders -XX:-UsePerfData -Xss512k \
  -XX:MaxDirectMemorySize=256m \
  --enable-native-access=ALL-UNNAMED --sun-misc-unsafe-memory-access=allow \
  -Dio.netty.leakDetection.level=disabled \
  -Dsqlite.lib="$PWD/build/libsqlite3.so" \
  -cp "target/server.jar:target/lib/*" challenge.Server
