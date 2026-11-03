#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Eclipse Temurin JDK 25.0.4.1+1 (official tarball, SHA-256
# pinned) under /opt, plus gcc and the tools build.sh uses to fetch SQLite. Yes, a whole JDK. I know.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip
JDK=OpenJDK25U-jdk_x64_linux_hotspot_25.0.4.1_1.tar.gz
SHA=dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e
curl -fsSL -o /tmp/$JDK https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.4.1%2B1/$JDK
echo "$SHA  /tmp/$JDK" | sha256sum -c -
rm -rf /opt/temurin-25 && mkdir -p /opt/temurin-25 && tar -C /opt/temurin-25 --strip-components=1 -xzf /tmp/$JDK && rm /tmp/$JDK
/opt/temurin-25/bin/java -version
