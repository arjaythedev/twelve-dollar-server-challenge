#!/usr/bin/env bash
# install.sh — runs once as root on a clean Ubuntu 24.04.
# Installs the Go toolchain (pinned) and a C compiler for cgo (go-sqlite3).
set -euo pipefail

GO_VERSION=1.24.4
GO_TARBALL=go${GO_VERSION}.linux-amd64.tar.gz
GO_SHA256=77e5da33bb72aeaef1ba4418b6fe511bc4d041873cbf82e5aa6318740df98717

apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl

curl -fsSLO "https://go.dev/dl/${GO_TARBALL}"
echo "${GO_SHA256}  ${GO_TARBALL}" | sha256sum -c -
rm -rf /usr/local/go
tar -C /usr/local -xzf "${GO_TARBALL}"
rm -f "${GO_TARBALL}"

/usr/local/go/bin/go version
