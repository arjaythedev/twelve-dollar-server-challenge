#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Go 1.27.2 (official tarball, SHA-256 pinned) plus a C
# compiler for cgo and the tools build.sh uses to fetch SQLite.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip
GO=go1.27.2.linux-amd64.tar.gz
SHA=ecbadb99091a3f46e31f5f934b068b1864eafa7995211b39eaddf76996045fe5
curl -fsSL -o /tmp/$GO https://go.dev/dl/$GO
echo "$SHA  /tmp/$GO" | sha256sum -c -
rm -rf /usr/local/go && tar -C /usr/local -xzf /tmp/$GO && rm /tmp/$GO
/usr/local/go/bin/go version
