#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
# - Graal JIT (this JDK's default): with a roomy heap it beat C2 by ~6-20% CPU per request in load tests.
# - Fixed 1 GB heap: half the box. A tight heap made GC (and cache eviction) cost CPU; it never resizes.
# - Serial GC: on one CPU, Parallel/G1 add GC threads that compete with requests.
# - Transparent huge pages (Ubuntu's default THP mode is madvise, so no kernel tuning): ~2.5% less CPU per request.
exec /opt/graalvm/bin/java -Xms1g -Xmx1g -XX:+UseSerialGC -XX:+UseTransparentHugePages -jar build/libs/feed-1.0.0.jar
