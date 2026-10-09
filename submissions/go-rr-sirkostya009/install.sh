#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C toolchain and SQLite's headers
# for cgo (Ubuntu's libsqlite3 3.45.1), and Go from the official tarball.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
GO_VERSION=1.27.2
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl libsqlite3-dev
rm -rf /usr/local/go
curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" | tar -C /usr/local -xz
/usr/local/go/bin/go version
