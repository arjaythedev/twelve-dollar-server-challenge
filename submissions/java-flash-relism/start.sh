#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
java_bin=java
if [[ -x /opt/flash-feed/jdk/bin/java ]]; then java_bin=/opt/flash-feed/jdk/bin/java; fi
exec "$java_bin" -Xms128m -Xmx1280m -XX:+UseSerialGC -XX:ActiveProcessorCount=1 \
    -Dorg.slf4j.simpleLogger.defaultLogLevel=warn -jar target/server.jar
