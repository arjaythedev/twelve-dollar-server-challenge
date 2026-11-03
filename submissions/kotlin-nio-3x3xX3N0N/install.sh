#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Temurin JDK 25 + Gradle 9.8.1 into /opt (pinned + sha256-checked).
set -euo pipefail
apt-get update && apt-get install -y curl unzip ca-certificates

JDK_URL=https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.4.1%2B1/OpenJDK25U-jdk_x64_linux_hotspot_25.0.4.1_1.tar.gz
JDK_SHA=dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e
GRADLE_URL=https://services.gradle.org/distributions/gradle-9.8.1-bin.zip
GRADLE_SHA=dce76f55f8e251a3a1f130eb120f30b3d271de2b76c9b0729d316b5a1b6dc01f

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL "$JDK_URL" -o "$tmp/jdk.tgz"
echo "$JDK_SHA  $tmp/jdk.tgz" | sha256sum -c -
rm -rf /opt/jdk-25 && mkdir -p /opt/jdk-25 && tar -xzf "$tmp/jdk.tgz" -C /opt/jdk-25 --strip-components=1

curl -fsSL "$GRADLE_URL" -o "$tmp/gradle.zip"
echo "$GRADLE_SHA  $tmp/gradle.zip" | sha256sum -c -
rm -rf /opt/gradle-9.8.1 && unzip -q "$tmp/gradle.zip" -d /opt

/opt/jdk-25/bin/java -version
