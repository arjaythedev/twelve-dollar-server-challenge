#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C toolchain for cgo (SQLite) and Go 1.27.2 from go.dev.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential make curl ca-certificates

GO_VERSION=1.27.2
case "$(uname -m)" in
  x86_64)  arch=amd64; sum=ecbadb99091a3f46e31f5f934b068b1864eafa7995211b39eaddf76996045fe5 ;;
  aarch64) arch=arm64; sum=94f3e30b8e374bc285e7dadc11e0865726b9bc6e85b841ccceaabc0214c6b7c8 ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac
tarball="go${GO_VERSION}.linux-${arch}.tar.gz"
curl -fsSL -o "/tmp/$tarball" "https://go.dev/dl/$tarball"
echo "$sum  /tmp/$tarball" | sha256sum -c -
rm -rf /usr/local/go
tar -C /usr/local -xzf "/tmp/$tarball"
rm "/tmp/$tarball"
/usr/local/go/bin/go version
