#!/usr/bin/env bash
# Builds the executable jar as a normal user. Gradle downloads the pinned dependencies.
set -euo pipefail
cd "$(dirname "$0")"
export JAVA_HOME=/opt/graalvm
bash ./gradlew --no-daemon --console=plain bootJar
