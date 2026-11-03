#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
# JVM flags for a 1 vCPU / 2 GB box: one GC thread, one "processor", a heap that fits, no perf file.
DIR="$(cd "$(dirname "$0")" && pwd)"
exec /opt/temurin-25/bin/java -XX:+UseSerialGC -Xms256m -Xmx512m -Xmn64m -XX:ActiveProcessorCount=1 \
  -XX:-UsePerfData -Xss512k --enable-native-access=ALL-UNNAMED -cp "$DIR/bin" Server
