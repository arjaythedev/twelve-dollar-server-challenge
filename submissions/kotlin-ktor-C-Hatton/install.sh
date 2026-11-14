#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: OpenJDK 25 (Ubuntu's package), gcc (to build SQLite from source)
# and Gradle 9.8.1 (official zip).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends openjdk-25-jdk-headless gcc libc6-dev curl unzip ca-certificates
GRADLE=9.8.1
GRADLE_SHA256=dce76f55f8e251a3a1f130eb120f30b3d271de2b76c9b0729d316b5a1b6dc01f
tmp="$(mktemp -d)"
curl -fsSL -o "$tmp/gradle.zip" "https://services.gradle.org/distributions/gradle-$GRADLE-bin.zip"
echo "$GRADLE_SHA256  $tmp/gradle.zip" | sha256sum -c -
rm -rf "/opt/gradle-$GRADLE"
unzip -q -d /opt "$tmp/gradle.zip"
rm -rf "$tmp"
java -version
/opt/gradle-$GRADLE/bin/gradle --version
