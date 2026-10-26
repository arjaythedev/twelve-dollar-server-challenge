#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
# The JDK install.sh put in /opt wins over a JAVA_HOME from the environment (CI runners set one to an older JDK).
if [ -x /opt/jdk-27/bin/java ]; then JAVA_HOME=/opt/jdk-27; fi
: "${JAVA_HOME:?no JDK in /opt/jdk-27 (run install.sh) and JAVA_HOME is not set}"
# Parallel GC: on one vCPU it runs one GC thread, stop-the-world, generational with adaptive young-generation sizing.
#   It was the only collector to hold 32,000 users locally (Serial, G1, ZGC and Shenandoah failed; see FINDINGS.md).
# Heap: 60% of the box's RAM (about 1.2 GB of 1,967 MB), fixed and pre-touched. The rest is for the OS, Nginx, the
#   JVM's own memory and the page cache holding the 240 MB database. There is no swap, so the heap must not grow later.
# Fiber tracing off: it keeps a ring buffer of events in every fiber.
SCALA_CLI=/opt/scala-cli/scala-cli; [ -x "$SCALA_CLI" ] || SCALA_CLI=scala-cli
# scala-cli runs the classes build.sh compiled (target/classpath.txt) without compiling them again: --java because
# the Scala library is already on that class path. It replaces itself with the JVM, so the server keeps its PID.
exec "$SCALA_CLI" run --server=false --java-home "$JAVA_HOME" --java -cp "$(cat target/classpath.txt)" \
  --main-class feed.Main \
  --java-opt=-XX:+UseParallelGC --java-opt=-XX:MaxRAMPercentage=60 --java-opt=-XX:InitialRAMPercentage=60 \
  --java-opt=-XX:+AlwaysPreTouch --java-opt=-Xss512k --java-opt=-XX:MaxDirectMemorySize=128m \
  --java-opt=-XX:-UsePerfData --java-opt=-Dcats.effect.tracing.mode=none --java-opt=--enable-native-access=ALL-UNNAMED \
  --java-opt=--sun-misc-unsafe-memory-access=allow
