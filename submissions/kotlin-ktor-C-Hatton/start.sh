#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
exec java \
  -XX:+UseSerialGC -Xms850m -Xmx850m -Xmn256m -XX:+AlwaysPreTouch \
  -XX:+UseCompactObjectHeaders -XX:MaxDirectMemorySize=256m -Xss512k \
  -Dio.netty.leakDetection.level=disabled \
  --enable-native-access=ALL-UNNAMED -Dsqlite.lib="$PWD/build/native/libsqlite3.so" \
  -cp 'build/install/server/lib/*' server.MainKt
