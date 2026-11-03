#!/usr/bin/env bash
# Once, as root, on a clean Ubuntu 24.04. Installs the JDK and a C compiler.
# The JDK is the GA build from https://jdk.java.net/27/ (OpenJDK 27.0.1.0.3).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates

# Pinned to the checksums published next to the tarballs.
BASE="https://download.java.net/java/GA/jdk27.0.1.0.3/b14170c9b2de463d9b89442c6db3e384/1/GPL"
case "$(uname -m)" in
  x86_64)
    FILE="openjdk-27.0.1.0.3_linux-x64_bin.tar.gz"
    SHA256=256199399aaddc6d66919b5de90794b4263e4ab312aa56692cb63b70a0abef41
    ;;
  aarch64)
    FILE="openjdk-27.0.1.0.3_linux-aarch64_bin.tar.gz"
    SHA256=a088a727fa3e76ebeadfa76ed9c1e9fecbdfa54ec752ecb669c38a8585e058a1
    ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
curl -fsSL "$BASE/$FILE" -o "$tmp"
echo "$SHA256  $tmp" | sha256sum -c -
rm -rf /opt/jdk
mkdir -p /opt/jdk
tar -xzf "$tmp" -C /opt/jdk --strip-components=1
/opt/jdk/bin/java -version
