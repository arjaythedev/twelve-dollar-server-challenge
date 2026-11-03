#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
exec /opt/jdk-25/bin/java \
  -XX:ActiveProcessorCount=1 -XX:+UseSerialGC \
  -Xms640m -Xmx640m -Xmn160m -XX:+AlwaysPreTouch \
  -XX:+UseCompactObjectHeaders \
  -XX:MaxMetaspaceSize=160m -XX:MaxDirectMemorySize=128m -XX:+DisableExplicitGC \
  -Xss512k \
  --enable-native-access=ALL-UNNAMED \
  --sun-misc-unsafe-memory-access=allow \
  --add-opens=java.base/java.nio=ALL-UNNAMED -Dio.netty.tryReflectionSetAccessible=true \
  -Dio.netty.allocator.type=pooled -Dio.netty.allocator.numDirectArenas=1 -Dio.netty.allocator.numHeapArenas=1 \
  -Dio.netty.leakDetection.level=disabled \
  -Dfeed.sqliteLib="$DIR/native/libfeedsqlite.so" \
  -jar "$DIR/target/feed.jar"
