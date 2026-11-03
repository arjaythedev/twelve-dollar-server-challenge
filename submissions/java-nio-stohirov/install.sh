#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates

JDK=jdk-25.0.4.1+1
case "$(uname -m)" in
  x86_64)  ARCH=x64;     SHA256=dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e ;;
  aarch64) ARCH=aarch64; SHA256=69df11a02cfa3ef7d7ca645e03edce6778ec090e100f6ae2b42097865730ac52 ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac
URL="https://github.com/adoptium/temurin25-binaries/releases/download/${JDK/+/%2B}/OpenJDK25U-jdk_${ARCH}_linux_hotspot_25.0.4.1_1.tar.gz"

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
curl -fsSL "$URL" -o "$tmp"
echo "$SHA256  $tmp" | sha256sum -c -
rm -rf /opt/jdk
mkdir -p /opt/jdk
tar -xzf "$tmp" -C /opt/jdk --strip-components=1
/opt/jdk/bin/java -version
