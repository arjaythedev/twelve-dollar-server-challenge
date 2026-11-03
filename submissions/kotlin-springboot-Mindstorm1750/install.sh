#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Oracle GraalVM for JDK 25 (a JDK with the Graal JIT compiler).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
GRAALVM_URL=https://download.oracle.com/graalvm/25/archive/graalvm-jdk-25.0.4_linux-x64_bin.tar.gz
GRAALVM_SHA256=76007c309f821aaf435bce63162ea0395587fc77350801c81643fe7feea37276

apt-get update
apt-get install -y --no-install-recommends curl ca-certificates

curl -fsSL -o /tmp/graalvm.tar.gz "$GRAALVM_URL"
echo "$GRAALVM_SHA256  /tmp/graalvm.tar.gz" | sha256sum -c -
rm -rf /opt/graalvm && mkdir -p /opt/graalvm
tar -xzf /tmp/graalvm.tar.gz -C /opt/graalvm --strip-components=1
rm /tmp/graalvm.tar.gz

/opt/graalvm/bin/java -version
