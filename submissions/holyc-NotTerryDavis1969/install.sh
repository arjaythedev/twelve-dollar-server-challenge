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

# hcc resolves #include <...> against /usr/local/include/ only, but on Ubuntu
# the glibc headers (pthread.h, features.h, bits/*, sys/*, …) live under
# /usr/include and the multiarch directory /usr/include/x86_64-linux-gnu.
# Symlink every system header into /usr/local/include so transitive includes
# resolve.  We never clobber hcc's own tos.HH (installed just above).
for d in /usr/include /usr/include/x86_64-linux-gnu; do
    for item in "$d"/*; do
        b=$(basename "$item")
        if [ -e "/usr/local/include/$b" ] || [ -L "/usr/local/include/$b" ]; then
            continue  # don't overwrite hcc headers like tos.HH
        fi
        ln -s "$item" "/usr/local/include/$b"
    done
done

echo "HolyC compiler installed successfully"