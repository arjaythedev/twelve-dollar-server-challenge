#!/usr/bin/env bash
# Run once as root on Ubuntu 24.04 x86_64. No global Java alternative is changed.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl tar gzip gcc libc6-dev python3
[[ $(uname -m) == x86_64 ]] || { echo 'This pinned installer targets x86_64.' >&2; exit 1; }
JDK=/opt/java-jdkhttpserver/jdk-25.0.4.1+1
if [[ ! -x "$JDK/bin/javac" ]]; then
  archive=$(mktemp)
  trap 'rm -f "$archive"' EXIT
  curl -fL --retry 3 'https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.4.1%2B1/OpenJDK25U-jdk_x64_linux_hotspot_25.0.4.1_1.tar.gz' -o "$archive"
  echo "dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e  $archive" | sha256sum -c -
  mkdir -p /opt/java-jdkhttpserver
  tar -xzf "$archive" -C /opt/java-jdkhttpserver
fi
"$JDK/bin/java" -version
