#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential ca-certificates curl

GO_VERSION=1.23.4
case "$(uname -m)" in
  x86_64)  GO_ARCH=amd64 ;;
  aarch64) GO_ARCH=arm64 ;;
  *) echo "unsupported arch $(uname -m)"; exit 1 ;;
esac

archive=$(mktemp)
trap 'rm -f "$archive"' EXIT
curl -fsSL --retry 3 "https://go.dev/dl/go${GO_VERSION}.linux-${GO_ARCH}.tar.gz" -o "$archive"
rm -rf /usr/local/go
tar -C /usr/local -xzf "$archive"
/usr/local/go/bin/go version
