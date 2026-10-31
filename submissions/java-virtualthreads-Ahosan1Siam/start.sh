#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"

# Run on 1 vCPU / 2 GB droplet with Serial GC, fixed heap and compact object headers
exec java \
  -XX:+UseSerialGC \
  -Xms256m \
  -Xmx512m \
  -XX:+AlwaysPreTouch \
  -XX:+UseCompactObjectHeaders \
  -jar "$DIR/build/server.jar"

