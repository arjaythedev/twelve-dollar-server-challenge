#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler and liburing (for cgo, SQLite and
# io_uring) and Go 1.27.2.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl unzip liburing-dev

GO_VERSION=1.27.2
case "$(uname -m)" in
  x86_64)  GO_ARCH=amd64; GO_SHA256=ecbadb99091a3f46e31f5f934b068b1864eafa7995211b39eaddf76996045fe5 ;;
  aarch64) GO_ARCH=arm64; GO_SHA256=94f3e30b8e374bc285e7dadc11e0865726b9bc6e85b841ccceaabc0214c6b7c8 ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac
tarball="$(mktemp)"
curl -fsSL -o "$tarball" "https://go.dev/dl/go${GO_VERSION}.linux-${GO_ARCH}.tar.gz"
echo "$GO_SHA256  $tarball" | sha256sum -c -
rm -rf /usr/local/go
tar -C /usr/local -xzf "$tarball"
rm -f "$tarball"
/usr/local/go/bin/go version
