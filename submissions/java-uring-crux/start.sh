#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk/bin/java ]; then JAVA_HOME=/opt/jdk; fi
LIB="$PWD/build/libsqlite3.so"
if [ "$(uname)" = Darwin ]; then LIB="$PWD/build/libsqlite3.dylib"; fi
# Serial GC: a second collector thread would compete for the one core.
# Compact headers are the JDK 27 default; the flag keeps the same layout on JDK 25.
exec "${JAVA_HOME:+$JAVA_HOME/bin/}java" \
  -XX:+UseSerialGC -Xms256m -Xmx768m -XX:+AlwaysPreTouch -XX:+UseCompactObjectHeaders \
  -XX:MaxDirectMemorySize=256m -XX:ReservedCodeCacheSize=64m -XX:MaxMetaspaceSize=64m \
  --enable-native-access=ALL-UNNAMED -Dsqlite.lib="$LIB" \
  -jar build/server.jar
