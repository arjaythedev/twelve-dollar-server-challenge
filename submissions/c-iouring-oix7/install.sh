#!/usr/bin/env bash
# Provision a fresh Ubuntu 24.04 host. Run as root: sudo ./install.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "install.sh must be run as root (sudo ./install.sh)" >&2
  exit 1
fi

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  build-essential ca-certificates curl unzip
