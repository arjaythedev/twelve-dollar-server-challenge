#!/usr/bin/env bash
set -euo pipefail
: "${SQLITE_PATH:?SQLITE_PATH must name a fresh copy of the seed database}"
: "${JWT_SECRET:?JWT_SECRET is required}"
JAVA=${JAVA:-/opt/java-challenge-25.0.1/bin/java}
exec "$JAVA" -Xmx1280m -XX:+UseSerialGC -jar "$(dirname "$0")/target/server.jar"
