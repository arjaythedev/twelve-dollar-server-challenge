#!/usr/bin/env bash
# Once, as root, on Ubuntu 24.04: pinned Temurin 27 and Maven under /opt (checksums verified), apt libsqlite3-0.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

TEMURIN_RELEASE="jdk-27+35"   # Temurin 27 GA (non-LTS feature release)
TEMURIN_FILE_VERSION="27_35"
MAVEN_VERSION="3.9.16"
MAVEN_SHA512="831a8591fe20c8243b1dbe7d71e3244f31d1665b0804b2e825e38cbbe5ce0cafb8338851f90780735568773e0a6cd07bbec107cda0b896b008b861075358b6f6"

case "$(uname -m)" in
  aarch64|arm64) ARCH=aarch64; TEMURIN_SHA256="e4ec5c7276290c7bde0baecba9a29be4962f5388684296406a9799d465936087" ;;
  x86_64|amd64)  ARCH=x64;     TEMURIN_SHA256="1cf69a4848ffb728b3b260dfd45206a51566ab571a02a30092271d4c580bccbc" ;;
  *) echo "unsupported architecture: $(uname -m)"; exit 1 ;;
esac

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends curl ca-certificates tar gzip libsqlite3-0

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# Temurin JDK -> /opt/temurin-27 (symlink to the versioned directory)
JDK_DIR="/opt/temurin-${TEMURIN_RELEASE#jdk-}"
if [ ! -x "$JDK_DIR/bin/java" ]; then
  url="https://github.com/adoptium/temurin27-binaries/releases/download/${TEMURIN_RELEASE/+/%2B}/OpenJDK27U-jdk_${ARCH}_linux_hotspot_${TEMURIN_FILE_VERSION}.tar.gz"
  curl -fsSL -o "$TMP/jdk.tar.gz" "$url"
  echo "$TEMURIN_SHA256  $TMP/jdk.tar.gz" | sha256sum -c -
  mkdir -p "$JDK_DIR"
  tar -xzf "$TMP/jdk.tar.gz" -C "$JDK_DIR" --strip-components=1
fi
ln -sfn "$JDK_DIR" /opt/temurin-27

# Maven -> /opt/maven (symlink to the versioned directory)
MVN_DIR="/opt/apache-maven-$MAVEN_VERSION"
if [ ! -x "$MVN_DIR/bin/mvn" ]; then
  curl -fsSL -o "$TMP/maven.tar.gz" \
    "https://archive.apache.org/dist/maven/maven-3/$MAVEN_VERSION/binaries/apache-maven-$MAVEN_VERSION-bin.tar.gz"
  echo "$MAVEN_SHA512  $TMP/maven.tar.gz" | sha512sum -c -
  mkdir -p "$MVN_DIR"
  tar -xzf "$TMP/maven.tar.gz" -C "$MVN_DIR" --strip-components=1
fi
ln -sfn "$MVN_DIR" /opt/maven

/opt/temurin-27/bin/java -version
ls /usr/lib/*-linux-gnu/libsqlite3.so.0
JAVA_HOME=/opt/temurin-27 /opt/maven/bin/mvn -v | head -1
