#!/usr/bin/env bash
# Builds target/quarkus-app/quarkus-run.jar.
set -euo pipefail
cd "$(dirname "$0")"
. ./env.sh
mvn -q -B package -DskipTests
