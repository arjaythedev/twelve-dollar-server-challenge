#!/usr/bin/env bash
# Builds as a normal user: compiles the app with sbt and writes the runtime classpath for start.sh.
set -euo pipefail
cd "$(dirname "$0")"
JAVA_HOME="${JAVA_HOME:-/opt/jdk-27}"
export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"
SBT="$(command -v /opt/sbt/bin/sbt || command -v sbt)"
# --server: one sbt JVM in the foreground that exits when done (sbt 2 otherwise starts a background server).
"$SBT" --server --batch compile writeClasspath
