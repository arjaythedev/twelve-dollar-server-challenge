#!/usr/bin/env bash
set -euo pipefail
JDK_RELEASE=jdk-26.0.2.1+1
JDK_BUILD=26.0.2.1_1
MAVEN_VERSION=3.10.0

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl libsqlite3-0

case "$(uname -m)" in
  x86_64)  arch=x64 ;;
  aarch64) arch=aarch64 ;;
  *) echo "unsupported architecture: $(uname -m)"; exit 1 ;;
esac

rm -rf /opt/jdk-26 /opt/maven && mkdir -p /opt/jdk-26 /opt/maven
curl -fsSL "https://github.com/adoptium/temurin26-binaries/releases/download/${JDK_RELEASE/+/%2B}/OpenJDK26U-jdk_${arch}_linux_hotspot_${JDK_BUILD}.tar.gz" \
  | tar -xz -C /opt/jdk-26 --strip-components=1
curl -fsSL "https://repo1.maven.org/maven2/org/apache/maven/apache-maven/$MAVEN_VERSION/apache-maven-$MAVEN_VERSION-bin.tar.gz" \
  | tar -xz -C /opt/maven --strip-components=1

/opt/jdk-26/bin/java -version
