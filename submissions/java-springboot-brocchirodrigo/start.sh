#!/usr/bin/env bash
cd "$(dirname "$0")"
JAVA=/opt/jdk-26/bin/java
[ -x "$JAVA" ] || JAVA="${JAVA_HOME:?}/bin/java"
exec "$JAVA" \
  -XX:+UseSerialGC \
  -XX:+UseCompactObjectHeaders \
  -Xms768m -Xmx768m -Xmn512m \
  --enable-native-access=ALL-UNNAMED \
  -jar target/app.jar
