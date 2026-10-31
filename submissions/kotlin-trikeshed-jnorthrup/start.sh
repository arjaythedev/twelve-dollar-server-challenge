#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
JAVA=/opt/graalvm/bin/java; [ -x "$JAVA" ] || JAVA="${JAVA_HOME:+$JAVA_HOME/bin/}java"
LIB="$PWD/server/build/sqlite/libsqlite3.so"; [ -f "$LIB" ] || LIB="$PWD/server/build/sqlite/libsqlite3.dylib"
exec "$JAVA" -XX:+UseSerialGC -Xms384m -Xmx384m -XX:-UsePerfData \
  --enable-native-access=ALL-UNNAMED -Dsqlite.lib="$LIB" \
  -cp "server/build/app/*" borg.trikeshed.feed.MainKt
