#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
# The JDK install.sh put in /opt wins over a JAVA_HOME from the environment (CI runners set one to an older JDK).
if [ -x /opt/jdk-27/bin/java ]; then JAVA_HOME=/opt/jdk-27; fi
# Parallel GC: on one vCPU it runs one GC thread, stop-the-world, generational with adaptive young-generation sizing.
#   It was the only collector to hold 32,000 users locally (Serial, G1, ZGC and Shenandoah failed; see FINDINGS.md).
# Heap: 60% of the box's RAM (about 1.2 GB of 1,967 MB), fixed and pre-touched. The rest is for the OS, Nginx, the
#   JVM's own memory and the page cache holding the 240 MB database. There is no swap, so the heap must not grow later.
# Fiber tracing off: it keeps a ring buffer of events in every fiber.
exec "$JAVA_HOME/bin/java" \
  -XX:+UseParallelGC -XX:MaxRAMPercentage=60 -XX:InitialRAMPercentage=60 -XX:+AlwaysPreTouch -Xss512k \
  -XX:MaxDirectMemorySize=128m -XX:-UsePerfData -Dcats.effect.tracing.mode=none \
  --enable-native-access=ALL-UNNAMED --sun-misc-unsafe-memory-access=allow \
  -cp "$(cat target/classpath.txt)" feed.Main
