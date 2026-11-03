#!/usr/bin/env bash
set -euo pipefail
if [[ $(uname -s) != Linux ]]; then
  echo 'Install JDK 25 on this OS; install.sh targets Ubuntu 24.04.' >&2
  exit 1
fi
apt-get update
apt-get install -y curl ca-certificates tar unzip
case "$(uname -m)" in x86_64) arch=x64;; aarch64) arch=aarch64;; *) echo 'Unsupported architecture' >&2; exit 1;; esac
if [[ ! -x /opt/temurin-25.0.2/bin/java ]]; then
  temp=$(mktemp -d)
  trap 'rm -rf "$temp"' EXIT
  asset="OpenJDK25U-jdk_${arch}_linux_hotspot_25.0.2_10.tar.gz"
  base='https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.2%2B10'
  curl -fL "$base/$asset" -o "$temp/$asset"
  curl -fL "$base/$asset.sha256.txt" -o "$temp/checksum"
  (cd "$temp" && sha256sum -c checksum)
  mkdir -p /opt/temurin-25.0.2
  tar -xzf "$temp/$asset" --strip-components=1 -C /opt/temurin-25.0.2
fi
update-alternatives --install /usr/bin/java java /opt/temurin-25.0.2/bin/java 2502
update-alternatives --install /usr/bin/javac javac /opt/temurin-25.0.2/bin/javac 2502
update-alternatives --set java /opt/temurin-25.0.2/bin/java
update-alternatives --set javac /opt/temurin-25.0.2/bin/javac
java -version
