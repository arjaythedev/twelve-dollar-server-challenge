#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler for cgo + SQLite, unzip, and Go.
set -euo pipefail
GO_VERSION=1.27.2
case "$(uname -m)" in
  x86_64)  GO_ARCH=amd64; GO_SHA256=ecbadb99091a3f46e31f5f934b068b1864eafa7995211b39eaddf76996045fe5 ;;
  aarch64) GO_ARCH=arm64; GO_SHA256=94f3e30b8e374bc285e7dadc11e0865726b9bc6e85b841ccceaabc0214c6b7c8 ;;
  *) echo "unsupported arch $(uname -m)"; exit 1 ;;
esac
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y build-essential curl unzip ca-certificates
tarball="$(mktemp)"
curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${GO_ARCH}.tar.gz" -o "$tarball"
echo "${GO_SHA256}  ${tarball}" | sha256sum -c -
rm -rf /usr/local/go
tar -C /usr/local -xzf "$tarball"
rm -f "$tarball"
/usr/local/go/bin/go version
