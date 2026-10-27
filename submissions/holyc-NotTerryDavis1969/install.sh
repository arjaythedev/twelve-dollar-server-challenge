#!/usr/bin/env bash
# HolyC $12 Server Challenge - install.sh
# Runs once as root on a clean Ubuntu 24.04
set -euo pipefail

# Install system dependencies
apt-get update
apt-get install -y --no-install-recommends \
    gcc \
    make \
    cmake \
    sqlite3 \
    libsqlite3-dev \
    openssl \
    libssl-dev \
    curl \
    wget \
    git

# Download and build HolyC compiler from source
cd /tmp
git clone https://github.com/Jamesbarford/holyc-lang.git holyc-src
cd holyc-src

# Build HolyC compiler with SQLite3 support
make -j$(nproc)
make install

# Verify installation
hcc --version

# Create symlinks if needed
if [ ! -x "$(command -v hcc)" ]; then
    ln -sf /usr/local/bin/hcc /usr/bin/hcc
fi

echo "HolyC compiler installed successfully"