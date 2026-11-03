#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export JAVA_HOME=/opt/java-challenge-25.0.1
export PATH="$JAVA_HOME/bin:$PATH"
MAVEN=/opt/maven-challenge-3.9.11/bin/mvn
"$MAVEN" --batch-mode --no-transfer-progress clean verify
