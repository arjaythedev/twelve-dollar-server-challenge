#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: build tools, Temurin JDK 25 (runs the Kotlin/Native compiler) and the
# Kotlin/Native 2.4.21 compiler, into /opt, pinned and sha256-checked. The server itself is a native binary: no JVM at
# run time. (build.sh fetches the pinned SQLite amalgamation, liburing and Kotlin/Native's own clang/sysroot bundle.)
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update && apt-get install -y --no-install-recommends curl unzip ca-certificates openssl make

JDK_URL=https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.4.1%2B1/OpenJDK25U-jdk_x64_linux_hotspot_25.0.4.1_1.tar.gz
JDK_SHA=dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e
KN_URL=https://github.com/JetBrains/kotlin/releases/download/v2.4.21/kotlin-native-prebuilt-linux-x86_64-2.4.21.tar.gz
KN_SHA=4935d8c8da90a2d2a8ff51da7d2c264c0d6026e1d35a8f169e218d9209965678

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
if [ ! -x /opt/jdk-25/bin/java ]; then
  curl -fsSL --retry 3 "$JDK_URL" -o "$tmp/jdk.tgz"
  echo "$JDK_SHA  $tmp/jdk.tgz" | sha256sum -c -
  rm -rf /opt/jdk-25 && mkdir -p /opt/jdk-25 && tar -xzf "$tmp/jdk.tgz" -C /opt/jdk-25 --strip-components=1
fi
if [ ! -x /opt/kotlin-native-2.4.21/bin/kotlinc-native ]; then
  curl -fsSL --retry 3 "$KN_URL" -o "$tmp/kn.tgz"
  echo "$KN_SHA  $tmp/kn.tgz" | sha256sum -c -
  rm -rf /opt/kotlin-native-2.4.21 && mkdir -p /opt/kotlin-native-2.4.21
  tar -xzf "$tmp/kn.tgz" -C /opt/kotlin-native-2.4.21 --strip-components=1
fi
/opt/jdk-25/bin/java -version
