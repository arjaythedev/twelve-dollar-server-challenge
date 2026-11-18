#!/usr/bin/env bash
# install.sh: runs once as root on a clean Ubuntu 24.04.
# Installs the C toolchain and dependencies needed to build BinaryCore.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential \
    curl \
    ca-certificates \
    libsqlite3-dev \
    sqlite3 \
    unzip
echo "install.sh: toolchain installed"
