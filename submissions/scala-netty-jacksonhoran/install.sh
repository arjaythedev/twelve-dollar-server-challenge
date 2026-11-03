#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler (to build SQLite from source),
# Temurin JDK 25 and sbt 1.13.0, both from their official release tarballs, checksum-verified.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gcc libc6-dev unzip

JDK_VERSION=25.0.4.1_1
case "$(uname -m)" in
  x86_64)  JDK_ARCH=x64;     JDK_SHA=dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e ;;
  aarch64) JDK_ARCH=aarch64; JDK_SHA=69df11a02cfa3ef7d7ca645e03edce6778ec090e100f6ae2b42097865730ac52 ;;
  *) echo "unsupported arch $(uname -m)"; exit 1 ;;
esac
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/jdk.tgz" "https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.4.1%2B1/OpenJDK25U-jdk_${JDK_ARCH}_linux_hotspot_${JDK_VERSION}.tar.gz"
echo "$JDK_SHA  $tmp/jdk.tgz" | sha256sum -c -
rm -rf /opt/jdk-25 && mkdir -p /opt/jdk-25 && tar -xzf "$tmp/jdk.tgz" -C /opt/jdk-25 --strip-components=1

curl -fsSL -o "$tmp/sbt.tgz" https://github.com/sbt/sbt/releases/download/v1.13.0/sbt-1.13.0.tgz
echo "06806805ffd26232727326216766ed4793b549f8c1e6ffeef2e610db7245b698  $tmp/sbt.tgz" | sha256sum -c -
rm -rf /opt/sbt && mkdir -p /opt/sbt && tar -xzf "$tmp/sbt.tgz" -C /opt/sbt --strip-components=1

/opt/jdk-25/bin/java -version
