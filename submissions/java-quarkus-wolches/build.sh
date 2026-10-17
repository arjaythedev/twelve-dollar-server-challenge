#!/usr/bin/env bash
# Builds the app as a normal user: a Quarkus fast-jar in target/quarkus-app.
# Maven downloads the dependencies pinned in pom.xml (Quarkus BOM 3.40.1) on the first run.
set -euo pipefail
cd "$(dirname "$0")"

export JAVA_HOME="${JAVA_HOME:-/opt/jdk-25}"
MVN="${MVN:-/opt/maven/bin/mvn}"

"$MVN" --batch-mode --no-transfer-progress -DskipTests clean package
ls -l target/quarkus-app/quarkus-run.jar
