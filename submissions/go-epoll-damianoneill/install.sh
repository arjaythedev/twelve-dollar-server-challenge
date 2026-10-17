#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C toolchain for cgo and the pinned Go release.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl

GO=go1.27.2
case "$(uname -m)" in
  x86_64) arch=amd64 sum=ecbadb99091a3f46e31f5f934b068b1864eafa7995211b39eaddf76996045fe5 ;;
  aarch64) arch=arm64 sum=94f3e30b8e374bc285e7dadc11e0865726b9bc6e85b841ccceaabc0214c6b7c8 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac
archive="$(mktemp)"; trap 'rm -f "$archive"' EXIT
curl -fsSL --retry 3 "https://go.dev/dl/$GO.linux-$arch.tar.gz" -o "$archive"
echo "$sum  $archive" | sha256sum -c - >/dev/null
rm -rf /usr/local/go
tar -C /usr/local -xzf "$archive"
/usr/local/go/bin/go version
