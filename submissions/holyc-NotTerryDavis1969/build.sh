#!/usr/bin/env bash
# HolyC $12 Server Challenge - build.sh
# Builds the optimized app as a normal user
set -euo pipefail

# Compile the HolyC server with maximum performance settings
# -O2 optimizations, preprocessor definitions for speed

HCC=$(command -v hcc || echo "hcc")

if [ "$HCC" = "hcc" ]; then
    echo "Error: HolyC compiler (hcc) not found"
    echo "Run install.sh first"
    exit 1
fi

echo "Compiling HolyC server with performance optimizations..."

cd "$(dirname "$0")"

# Compile with hcc using optimization flags
# -DHCC_LINK_SQLITE3=1 for SQLite support
# -lssl -lcrypto for networking
# Additional flags for performance

$HCC server.HC -o server \
    -O2 \
    -I/usr/local/include \
    -I/holyc-lang/src \
    -I/holyc-lang/src/holyc-lib \
    -DHCC_LINK_SQLITE3=1 \
    -DHCC_ENABLE_JIT=on \
    -lssl -lcrypto -lm \
    -Wl,-enable-threads

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

echo "Server built successfully: ./server (optimized)"
chmod +x server