#!/usr/bin/env bash
# Installs the toolchain on a clean Ubuntu 24.04. Run once as root; safe to re-run.
#   - apt: build-essential (GCC for SQLite), curl, ca-certificates
#   - Temurin JDK 25.0.4.1+1 -> /opt/jdk-25 (x86_64 or aarch64), sha256-verified
#   - Apache Maven 3.9.16   -> /opt/maven, sha512-verified
set -euo pipefail

JDK_DIR=/opt/jdk-25
JDK_BASE=https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.4.1%2B1
MAVEN_DIR=/opt/maven
MAVEN_VERSION=3.9.16
MAVEN_URL=https://archive.apache.org/dist/maven/maven-3/$MAVEN_VERSION/binaries/apache-maven-$MAVEN_VERSION-bin.tar.gz
MAVEN_SHA512=831a8591fe20c8243b1dbe7d71e3244f31d1665b0804b2e825e38cbbe5ce0cafb8338851f90780735568773e0a6cd07bbec107cda0b896b008b861075358b6f6

case "$(uname -m)" in
  x86_64)
    JDK_TAR=OpenJDK25U-jdk_x64_linux_hotspot_25.0.4.1_1.tar.gz
    JDK_SHA256=dbb698396d478e7fa2b1e50f4103324b2a99b90569ee27c33f2261f9215cf41e ;;
  aarch64|arm64)
    JDK_TAR=OpenJDK25U-jdk_aarch64_linux_hotspot_25.0.4.1_1.tar.gz
    JDK_SHA256=69df11a02cfa3ef7d7ca645e03edce6778ec090e100f6ae2b42097865730ac52 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# install_tarball <dir> <url> <sha256|sha512> <checksum>
# The checksum is stored in <dir>/.installed, so a re-run with the same pin does nothing.
install_tarball() {
  local dir=$1 url=$2 algo=$3 sum=$4
  if [ "$(cat "$dir/.installed" 2>/dev/null || true)" = "$sum" ]; then
    echo "$dir: already installed"
    return
  fi
  echo "$dir: downloading $url"
  curl -fsSL --retry 3 -o "$TMP/pkg.tar.gz" "$url"
  echo "$sum  $TMP/pkg.tar.gz" | "${algo}sum" -c --quiet -
  rm -rf "$dir.new"
  mkdir -p "$dir.new"
  tar -xzf "$TMP/pkg.tar.gz" -C "$dir.new" --strip-components=1
  echo "$sum" > "$dir.new/.installed"
  rm -rf "$dir"
  mv "$dir.new" "$dir"
  rm -f "$TMP/pkg.tar.gz"
}

install_tarball "$JDK_DIR" "$JDK_BASE/$JDK_TAR" sha256 "$JDK_SHA256"
install_tarball "$MAVEN_DIR" "$MAVEN_URL" sha512 "$MAVEN_SHA512"

"$JDK_DIR/bin/java" -version
JAVA_HOME="$JDK_DIR" "$MAVEN_DIR/bin/mvn" -v
