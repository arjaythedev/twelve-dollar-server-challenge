#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
source ./java-env.sh
export MALLOC_ARENA_MAX=1
exec "$JAVA_HOME/bin/java" \
  --enable-native-access=ALL-UNNAMED \
  -XX:ActiveProcessorCount=1 \
  -Xms512m -Xmx1024m -Xmn256m -XX:+AlwaysPreTouch \
  -XX:+UseSerialGC -XX:+UseCompactObjectHeaders \
  -Djdk.virtualThreadScheduler.parallelism=2 \
  -jar target/server.jar
