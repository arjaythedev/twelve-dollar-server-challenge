#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler (to build SQLite from source),
# Temurin JDK 25 and Maven, from pinned, checksum-verified tarballs.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gcc libc6-dev

JDK_VERSION=25.0.4.1_1
case "$(dpkg --print-architecture)" in
  amd64) JDK_ARCH=x64;     JDK_SHA256=dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e ;;
  arm64) JDK_ARCH=aarch64; JDK_SHA256=69df11a02cfa3ef7d7ca645e03edce6778ec090e100f6ae2b42097865730ac52 ;;
  *) echo "unsupported architecture"; exit 1 ;;
esac
MAVEN_VERSION=3.9.16
MAVEN_SHA512=831a8591fe20c8243b1dbe7d71e3244f31d1665b0804b2e825e38cbbe5ce0cafb8338851f90780735568773e0a6cd07bbec107cda0b896b008b861075358b6f6

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/jdk.tar.gz" \
  "https://github.com/adoptium/temurin25-binaries/releases/download/jdk-${JDK_VERSION/_/%2B}/OpenJDK25U-jdk_${JDK_ARCH}_linux_hotspot_${JDK_VERSION}.tar.gz"
echo "$JDK_SHA256  $tmp/jdk.tar.gz" | sha256sum -c -
curl -fsSL -o "$tmp/maven.tar.gz" \
  "https://archive.apache.org/dist/maven/maven-3/${MAVEN_VERSION}/binaries/apache-maven-${MAVEN_VERSION}-bin.tar.gz"
echo "$MAVEN_SHA512  $tmp/maven.tar.gz" | sha512sum -c -

rm -rf /opt/jdk-25 /opt/maven
mkdir -p /opt/jdk-25 /opt/maven
tar -xzf "$tmp/jdk.tar.gz" -C /opt/jdk-25 --strip-components=1
tar -xzf "$tmp/maven.tar.gz" -C /opt/maven --strip-components=1
/opt/jdk-25/bin/java -version
JAVA_HOME=/opt/jdk-25 /opt/maven/bin/mvn -v
