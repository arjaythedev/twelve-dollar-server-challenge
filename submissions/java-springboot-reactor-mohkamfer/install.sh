#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs a C compiler (build.sh compiles SQLite from
# source), plus Eclipse Temurin JDK 25 and Gradle 9.7.1 from their official tarballs (checksums pinned).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

JDK_VERSION=25.0.4.1+1
JDK_URL="https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.4.1%2B1/OpenJDK25U-jdk_x64_linux_hotspot_25.0.4.1_1.tar.gz"
JDK_SHA256=dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e
GRADLE_VERSION=9.7.1
GRADLE_URL="https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip"
GRADLE_SHA256=acd53f1edaf02f1a8ff99879f8a34b302661a057d9b063ae9e35b552f804d20a

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl unzip gcc libc6-dev

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

curl -fsSL -o "$tmp/jdk.tar.gz" "$JDK_URL"
echo "$JDK_SHA256  $tmp/jdk.tar.gz" | sha256sum -c -
rm -rf /opt/jdk && mkdir -p /opt/jdk
tar -xzf "$tmp/jdk.tar.gz" -C /opt/jdk --strip-components=1

curl -fsSL -o "$tmp/gradle.zip" "$GRADLE_URL"
echo "$GRADLE_SHA256  $tmp/gradle.zip" | sha256sum -c -
rm -rf /opt/gradle "/opt/gradle-$GRADLE_VERSION"
unzip -q "$tmp/gradle.zip" -d /opt
mv "/opt/gradle-$GRADLE_VERSION" /opt/gradle

/opt/jdk/bin/java -version
echo "installed Temurin $JDK_VERSION and Gradle $GRADLE_VERSION"
