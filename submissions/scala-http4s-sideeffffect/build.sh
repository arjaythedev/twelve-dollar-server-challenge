#!/usr/bin/env bash
# Builds as a normal user: compiles the app with scala-cli and writes its runtime classpath to target/classpath.txt,
# which start.sh runs. The Scala version and the pinned dependencies are in project.scala; they come from Maven Central.
set -euo pipefail
cd "$(dirname "$0")"
# The JDK install.sh put in /opt wins over a JAVA_HOME from the environment (CI runners set one to an older JDK).
if [ -x /opt/jdk-27/bin/java ]; then export JAVA_HOME=/opt/jdk-27; fi
export PATH="$JAVA_HOME/bin:$PATH"
SCALA_CLI=/opt/scala-cli/scala-cli; [ -x "$SCALA_CLI" ] || SCALA_CLI=scala-cli
mkdir -p target
# --server=false: compile in this process, with the JDK above; no Bloop build server is left running.
"$SCALA_CLI" compile --server=false --print-class-path src project.scala > target/classpath.txt
