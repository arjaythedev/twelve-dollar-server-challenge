#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs build tools and the latest Go toolchain.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential curl ca-certificates

GO_VERSION="1.27.2"
rm -rf /usr/local/go
curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" | tar -C /usr/local -xz
ln -sf /usr/local/go/bin/go /usr/local/bin/go
ln -sf /usr/local/go/bin/gofmt /usr/local/bin/gofmt

go version
