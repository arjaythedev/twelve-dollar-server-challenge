#!/usr/bin/env bash
# Runs the server in the foreground. Config comes from SQLITE_PATH, JWT_SECRET, HOST and PORT.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
# one glibc malloc arena: SQLite and the JVM's native allocations don't need per-thread arenas
export MALLOC_ARENA_MAX=1
exec /opt/jdk/bin/java \
  -XX:+UseSerialGC \
  -Xms32m -Xmx320m -Xmn24m \
  -XX:+UseCompactObjectHeaders \
  -XX:MaxMetaspaceSize=128m \
  -XX:ReservedCodeCacheSize=48m \
  -XX:MaxDirectMemorySize=256m \
  -Xss512k \
  -Dreactor.netty.ioWorkerCount=1 \
  -Dio.netty.allocator.numDirectArenas=1 \
  -Dio.netty.allocator.numHeapArenas=1 \
  -Dio.netty.leakDetection.level=disabled \
  --enable-native-access=ALL-UNNAMED \
  -Dapp.sqliteLib="$DIR/build/native/libsqlite3.so" \
  -jar "$DIR/build/libs/app.jar"
