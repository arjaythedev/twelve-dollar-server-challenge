#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk/bin/java ]; then JAVA_HOME=/opt/jdk; fi
LIB="$PWD/build/libsqlite3.so"
if [ "$(uname)" = Darwin ]; then LIB="$PWD/build/libsqlite3.dylib"; fi
exec "${JAVA_HOME:+$JAVA_HOME/bin/}java" \
  -XX:+UseSerialGC -Xms256m -Xmx256m -Xmn128m -XX:+AlwaysPreTouch -XX:+UseCompactObjectHeaders \
  -XX:MaxDirectMemorySize=64m -XX:ReservedCodeCacheSize=64m -XX:MaxMetaspaceSize=64m \
  --enable-native-access=ALL-UNNAMED -Dsqlite.lib="$LIB" \
  -jar build/server.jar
