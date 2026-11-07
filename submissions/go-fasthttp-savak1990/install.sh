#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: C toolchain for cgo + SQLite, and Go 1.27.1.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential curl unzip ca-certificates

GO_VER=1.27.1
GO_SHA256=63d339f0da5ab53635a56f2490a7984dfe12dfcff22ad749f63edaf590168445
if [ ! -x /usr/local/go/bin/go ] || [ "$(/usr/local/go/bin/go version | awk '{print $3}')" != "go$GO_VER" ]; then
  tmp="$(mktemp -d)"
  curl -fsSL "https://go.dev/dl/go${GO_VER}.linux-amd64.tar.gz" -o "$tmp/go.tgz"
  echo "$GO_SHA256  $tmp/go.tgz" | sha256sum -c -
  rm -rf /usr/local/go && tar -C /usr/local -xzf "$tmp/go.tgz"
  rm -rf "$tmp"
fi
/usr/local/go/bin/go version
