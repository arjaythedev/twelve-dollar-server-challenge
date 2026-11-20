#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the C toolchain, python3 (build.sh's profile
# training), and OpenJDK 27.0.1, the official GA tarball from jdk.java.net, sha256-checked
# against the checksums published beside it, unpacked into /opt/jdk-27.0.1.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip python3

JDK=27.0.1.0.3
BASE=https://download.java.net/java/GA/jdk27.0.1.0.3/b14170c9b2de463d9b89442c6db3e384/1/GPL
case "$(uname -m)" in
  x86_64)  ARCH=x64;     SHA256=256199399aaddc6d66919b5de90794b4263e4ab312aa56692cb63b70a0abef41 ;;
  aarch64) ARCH=aarch64; SHA256=a088a727fa3e76ebeadfa76ed9c1e9fecbdfa54ec752ecb669c38a8585e058a1 ;;
  *) echo "unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
prefix=/opt/jdk-27.0.1
if [[ ! -x $prefix/bin/javac ]]; then
  archive=$(mktemp)
  trap 'rm -f "$archive"' EXIT
  curl -fsSL --retry 3 -o "$archive" "$BASE/openjdk-${JDK}_linux-${ARCH}_bin.tar.gz"
  echo "$SHA256  $archive" | sha256sum -c -
  rm -rf "$prefix"
  mkdir -p "$prefix"
  tar -xzf "$archive" --strip-components=1 -C "$prefix"
fi
"$prefix/bin/java" -version
