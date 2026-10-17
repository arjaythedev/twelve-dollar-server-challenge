#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk-26/bin/java ]; then export JAVA_HOME=/opt/jdk-26; fi
MVN=/opt/maven/bin/mvn
[ -x "$MVN" ] || MVN=mvn
"$MVN" -B -q -DskipTests package
