#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs a pinned Temurin JDK 25 and Apache Maven into /opt.
# Both archives are verified against the checksums published by their projects.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

# The benchmark box and CI are x86_64; aarch64 is only here so the scripts can be rehearsed on an ARM machine.
case "$(uname -m)" in
  x86_64)
    JDK_URL="https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.3%2B9/OpenJDK25U-jdk_x64_linux_hotspot_25.0.3_9.tar.gz"
    JDK_SHA256="69264a7a211bf5029830d07bc3370f879769d62ebc5b5488e90c9343a2da0e1f" ;;
  aarch64)
    JDK_URL="https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.3%2B9/OpenJDK25U-jdk_aarch64_linux_hotspot_25.0.3_9.tar.gz"
    JDK_SHA256="3e4287cb98870ba824ed698854bdc27cff984254caf66dd12cc291e7bfdde26b" ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac
JDK_HOME="/opt/jdk-25"

MAVEN_URL="https://archive.apache.org/dist/maven/maven-3/3.9.16/binaries/apache-maven-3.9.16-bin.tar.gz"
MAVEN_SHA512="831a8591fe20c8243b1dbe7d71e3244f31d1665b0804b2e825e38cbbe5ce0cafb8338851f90780735568773e0a6cd07bbec107cda0b896b008b861075358b6f6"
MAVEN_HOME="/opt/maven"

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl tar gzip

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# fetch <url> <file> <sha256|sha512> <checksum>
fetch() {
  curl -fsSL --retry 3 --retry-delay 2 -o "$2" "$1"
  echo "$4  $2" | "${3}sum" -c - >/dev/null
}

fetch "$JDK_URL" "$tmp/jdk.tar.gz" sha256 "$JDK_SHA256"
rm -rf "$JDK_HOME" && mkdir -p "$JDK_HOME"
tar -xzf "$tmp/jdk.tar.gz" -C "$JDK_HOME" --strip-components=1

fetch "$MAVEN_URL" "$tmp/maven.tar.gz" sha512 "$MAVEN_SHA512"
rm -rf "$MAVEN_HOME" && mkdir -p "$MAVEN_HOME"
tar -xzf "$tmp/maven.tar.gz" -C "$MAVEN_HOME" --strip-components=1

"$JDK_HOME/bin/java" -version
JAVA_HOME="$JDK_HOME" "$MAVEN_HOME/bin/mvn" -v
