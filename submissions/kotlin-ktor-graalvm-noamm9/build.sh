#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

export JAVA_HOME=/opt/graalvm

bash ./gradlew --no-daemon clean installDist

# runs after Gradle has exited: on the 1 vCPU / 2 GB box Gradle and native-image don't fit in memory together
mkdir -p build/native/nativeCompile
/opt/graalvm/bin/native-image -J-Xmx1150m -J-XX:ReservedCodeCacheSize=64m -J-XX:MaxMetaspaceSize=384m \
    -cp "$(ls build/install/velox/lib/*.jar | paste -sd: -)" \
    -o build/native/nativeCompile/velox \
    dev.noamm9.velox.MainKt
