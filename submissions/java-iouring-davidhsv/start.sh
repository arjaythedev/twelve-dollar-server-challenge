#!/usr/bin/env bash
# Runs the server in the foreground, configured by SQLITE_PATH, JWT_SECRET, HOST and PORT.
# bin/server is the JDK's java launcher under another name. The heap is a safety net: the loop
# allocates nothing once it runs, so it stays small and Serial (no collector threads).
dir="$(cd "$(dirname "$0")" && pwd)"
exec "$dir/bin/server" \
  -XX:+UseSerialGC -Xms64m -Xmx64m -Xss1m \
  -XX:MaxMetaspaceSize=32m -XX:ReservedCodeCacheSize=48m -XX:MaxDirectMemorySize=16m \
  -XX:-UsePerfData -XX:+DisableAttachMechanism \
  ${FEED_JVM_FLAGS:-} \
  --enable-native-access=ALL-UNNAMED -Dfeed.lib="$dir/bin/libfeed.so" \
  -cp "$dir/.build/classes" Server
