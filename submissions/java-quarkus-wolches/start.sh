#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT (see SPEC.md);
# HOST, PORT and SQLITE_PATH are read by application.properties directly.
set -euo pipefail
cd "$(dirname "$0")"

# smallrye-jwt takes an HS256 secret only as an "oct" JWK, so JWT_SECRET is wrapped here:
# k = base64url(secret) without padding, the same bytes test/test.sh and bench/load.js sign with.
secret="${JWT_SECRET:-twelve-dollar-challenge}"
k="$(printf '%s' "$secret" | base64 -w0 | tr '+/' '-_' | tr -d '=')"
export SMALLRYE_JWT_VERIFY_SECRETKEY="{\"kty\":\"oct\",\"k\":\"$k\"}"

# The benchmark box has 1 vCPU and 2 GB RAM shared with the OS (and Nginx): a bounded heap and the
# single-threaded collector. Override with JAVA_OPTS if needed.
JAVA_OPTS="${JAVA_OPTS:--Xms512m -Xmx1g -XX:+UseSerialGC}"

JAVA="${JAVA_HOME:-/opt/jdk-25}/bin/java"
# shellcheck disable=SC2086
exec "$JAVA" $JAVA_OPTS --enable-native-access=ALL-UNNAMED -jar target/quarkus-app/quarkus-run.jar
