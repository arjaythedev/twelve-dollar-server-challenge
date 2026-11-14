#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the Go toolchain and the
# C toolchain + SQLite headers (the server uses cgo and links libsqlite3).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
    build-essential ca-certificates curl libsqlite3-dev

if ! command -v go >/dev/null 2>&1 && [ ! -x /usr/local/go/bin/go ]; then
    curl -fsSL https://go.dev/dl/go1.27.1.linux-amd64.tar.gz -o /tmp/go.tar.gz
    rm -rf /usr/local/go
    tar -C /usr/local -xzf /tmp/go.tar.gz
    rm -f /tmp/go.tar.gz
fi

/usr/local/go/bin/go version 2>/dev/null || go version
