#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk/bin/java ]; then JAVA_HOME=/opt/jdk; fi
exec "${JAVA_HOME:+$JAVA_HOME/bin/}java" \
  -XX:+UseSerialGC -Xms128m -Xmx128m -Xmn64m -XX:+AlwaysPreTouch -XX:+UseCompactObjectHeaders -XX:-UsePerfData \
  -XX:MaxDirectMemorySize=64m -XX:ReservedCodeCacheSize=64m -XX:MaxMetaspaceSize=64m \
  --enable-native-access=ALL-UNNAMED -Dshim.lib="$PWD/build/libshim.so" \
  -jar build/server.jar
