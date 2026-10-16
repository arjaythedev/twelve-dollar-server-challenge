#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04 x86_64.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential ca-certificates curl unzip
archive=$(mktemp); trap 'rm -f "$archive"' EXIT
curl -fsSL --retry 3 https://go.dev/dl/go1.27.2.linux-amd64.tar.gz -o "$archive"
rm -rf /usr/local/go
tar -C /usr/local -xzf "$archive"
/usr/local/go/bin/go version
