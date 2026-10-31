#!/usr/bin/env bash
set -euo pipefail
case "$(uname -m)" in
  x86_64)
    JAVA_ARCH=x64
    JAVA_SHA256=8daf77d1aacffe38c9889689bc224a13557de77559d9a5bb91991e6a298baa0d
    ;;
  aarch64|arm64)
    JAVA_ARCH=aarch64
    JAVA_SHA256=5c83b7d2121ed482fd06831a1eba1633dbab818aba6addddf48e075b97e6e9b8
    ;;
  *) echo "Unsupported Linux architecture: $(uname -m)" >&2; exit 1 ;;
esac
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl tar gzip

JAVA_DIR=/opt/java-challenge-25.0.1
MAVEN_DIR=/opt/maven-challenge-3.9.11
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -f "$JAVA_DIR/.installed" ]; then
  curl --fail --location --retry 3 --retry-all-errors --connect-timeout 30 --max-time 300 \
    "https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.1%2B8/OpenJDK25U-jdk_${JAVA_ARCH}_linux_hotspot_25.0.1_8.tar.gz" \
    -o "$TMP/java.tar.gz"
  echo "$JAVA_SHA256  $TMP/java.tar.gz" | sha256sum -c -
  mkdir -p "$TMP/java" "$JAVA_DIR"
  tar -xzf "$TMP/java.tar.gz" -C "$TMP/java" --strip-components=1 --no-same-owner
  cp -a "$TMP/java/." "$JAVA_DIR/"
  touch "$JAVA_DIR/.installed"
fi

if [ ! -f "$MAVEN_DIR/.installed" ]; then
  curl --fail --location --retry 3 --retry-all-errors --connect-timeout 30 --max-time 300 \
    'https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/3.9.11/apache-maven-3.9.11-bin.tar.gz' \
    -o "$TMP/maven.tar.gz"
  echo "bcfe4fe305c962ace56ac7b5fc7a08b87d5abd8b7e89027ab251069faebee516b0ded8961445d6d91ec1985dfe30f8153268843c89aa392733d1a3ec956c9978  $TMP/maven.tar.gz" | sha512sum -c -
  mkdir -p "$TMP/maven" "$MAVEN_DIR"
  tar -xzf "$TMP/maven.tar.gz" -C "$TMP/maven" --strip-components=1 --no-same-owner
  cp -a "$TMP/maven/." "$MAVEN_DIR/"
  touch "$MAVEN_DIR/.installed"
fi
