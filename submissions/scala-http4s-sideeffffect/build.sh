#!/usr/bin/env bash
# Builds as a normal user: compiles the app with sbt and writes the runtime classpath for start.sh.
set -euo pipefail
cd "$(dirname "$0")"
# The JDK install.sh put in /opt wins over a JAVA_HOME from the environment (CI runners set one to an older JDK).
if [ -x /opt/jdk-27/bin/java ]; then export JAVA_HOME=/opt/jdk-27; fi
export PATH="$JAVA_HOME/bin:$PATH"
SBT="$(command -v /opt/sbt/bin/sbt || command -v sbt)"
# --server: one sbt JVM in the foreground that exits when done (sbt 2 otherwise starts a background server).
"$SBT" --server --batch compile writeClasspath
