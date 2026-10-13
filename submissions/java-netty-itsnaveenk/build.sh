#!/usr/bin/env bash
# build.sh: Builds the app as a normal user.
set -euo pipefail

cd "$(dirname "$0")"
mvn clean package -DskipTests --batch-mode --no-transfer-progress
