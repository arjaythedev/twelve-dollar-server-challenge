#!/usr/bin/env bash
# Runs the server in the foreground. Config comes only from the env vars in SPEC.md:
# SQLITE_PATH, JWT_SECRET, HOST, PORT (defaults: 0.0.0.0:80, i.e. serving directly).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"

JDK_HOME="${JDK_HOME:-/opt/jdk-27}"
if [ ! -x "$JDK_HOME/bin/java" ]; then JDK_HOME="${JAVA_HOME:?no /opt/jdk-27 and no JAVA_HOME}"; fi

if [ "$(uname -s)" = Linux ]; then
  LIB="$DIR/native/libsqlite3.so"
  # Tuning for the benchmark box: 1 vCPU, 2 GB, no swap (README.md explains every flag).
  FLAGS=(
    -XX:+UseSerialGC
    -Xms384m -Xmx384m -Xmn128m
    -XX:+AlwaysPreTouch
    -XX:+UseTransparentHugePages
    -XX:+UseCompactObjectHeaders
    -XX:MaxDirectMemorySize=256m
    -XX:ReservedCodeCacheSize=64m
    -XX:MaxMetaspaceSize=64m
    -Xss256k
    -XX:-UsePerfData
  )
else
  # Local development on another OS: only the flags that exist everywhere.
  LIB="$DIR/native/libsqlite3.dylib"
  FLAGS=(-XX:+UseSerialGC -Xms384m -Xmx384m -Xmn128m -Xss256k -XX:-UsePerfData)
fi

exec "$JDK_HOME/bin/java" \
  "${FLAGS[@]}" \
  -Xlog:disable -Xlog:all=warning:stderr \
  -Djdk.virtualThreadScheduler.parallelism=1 \
  -Djdk.virtualThreadScheduler.maxPoolSize=2 \
  -Djdk.trackAllThreads=false \
  -Djava.net.preferIPv4Stack=true \
  -Dapp.sqliteLib="$LIB" \
  --enable-native-access=ALL-UNNAMED \
  -cp "$DIR/out" app.Main
