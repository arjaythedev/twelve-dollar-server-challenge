#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C toolchain for cgo (SQLite itself is compiled in by
# build.sh), and Go from the official tarball, checked against its sha256.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
GO_VERSION=1.27.2
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates

arch="$(dpkg --print-architecture)" # amd64 on the benchmark box; arm64 for local testing
case "$arch" in
  amd64) sha256=ecbadb99091a3f46e31f5f934b068b1864eafa7995211b39eaddf76996045fe5 ;;
  arm64) sha256=94f3e30b8e374bc285e7dadc11e0865726b9bc6e85b841ccceaabc0214c6b7c8 ;;
  *) echo "unsupported architecture: $arch" >&2; exit 1 ;;
esac
tarball="go$GO_VERSION.linux-$arch.tar.gz"
curl -fsSLo "/tmp/$tarball" "https://go.dev/dl/$tarball"
echo "$sha256  /tmp/$tarball" | sha256sum -c -
rm -rf /usr/local/go
tar -C /usr/local -xzf "/tmp/$tarball"
rm "/tmp/$tarball"
/usr/local/go/bin/go version
