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
    git \
    pkg-config \
    libpthread-stubs0-dev

# Download and build HolyC compiler from source
cd /tmp
git clone https://github.com/Jamesbarford/holyc-lang.git holyc-src
cd holyc-src

# Build HolyC compiler with SQLite3 support
make -j$(nproc)
make install

# Create symlinks if needed
if [ ! -x "$(command -v hcc)" ]; then
    ln -sf /usr/local/bin/hcc /usr/bin/hcc
fi

# Ensure pthread.h is available - hcc looks in /usr/local/include/
# but pthread.h is in /usr/include on Ubuntu
if [ ! -f /usr/local/include/pthread.h ]; then
    # Copy or symlink pthread.h to where hcc expects it
    if [ -f /usr/include/pthread.h ]; then
        ln -sf /usr/include/pthread.h /usr/local/include/pthread.h
    fi
    # Also make sure other common headers are accessible
    for hdr in stdio.h stdlib.h string.h stdbool.h; do
        if [ -f "/usr/include/$hdr" ] && [ ! -f "/usr/local/include/$hdr" ]; then
            ln -sf "/usr/include/$hdr" "/usr/local/include/$hdr"
        fi
    done
fi

echo "HolyC compiler installed successfully"