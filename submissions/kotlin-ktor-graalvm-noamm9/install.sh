#!/usr/bin/env bash
set -euo pipefail

GRAALVM_URL="https://github.com/graalvm/graalvm-ce-builds/releases/download/graal-25.4.4.1.1/graalvm-community-jdk-25i4-25.0.4.1.1_linux-x64_bin.tar.gz"
GRAALVM_SHA256="05ccbbe783210b6886ff7b08fcd0b061c5dce4852b05db87284fc0e24abb08e2"

apt-get update
apt-get install -y build-essential zlib1g-dev curl ca-certificates

curl -fsSL -o /tmp/graalvm.tar.gz "$GRAALVM_URL"
echo "$GRAALVM_SHA256  /tmp/graalvm.tar.gz" | sha256sum -c -

rm -rf /opt/graalvm
mkdir -p /opt/graalvm
tar -xzf /tmp/graalvm.tar.gz -C /opt/graalvm --strip-components=1
rm /tmp/graalvm.tar.gz
