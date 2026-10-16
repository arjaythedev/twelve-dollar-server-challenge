#!/usr/bin/env bash
# Runs the server in the foreground (SPEC.md env vars; JAVA_OPTS is appended last).
set -euo pipefail
cd "$(dirname "$0")"
. ./env.sh

exec java \
  -Xms256m -Xmx1g -Xmn384m \
  -XX:+UseG1GC -XX:MaxGCPauseMillis=100 \
  -XX:MaxDirectMemorySize=128m \
  -Dquarkus.http.host="${HOST:-0.0.0.0}" \
  -Dquarkus.http.port="${PORT:-3000}" \
  -Dquarkus.vertx.event-loops-pool-size="$(nproc)" \
  ${JAVA_OPTS:-} \
  -jar target/quarkus-app/quarkus-run.jar
