#!/usr/bin/env bash
# HolyC $12 Server Challenge - build.sh
# Builds the app as a normal user (may download pinned dependencies)
set -euo pipefail

# Compile the HolyC server using the hcc compiler
HCC=$(command -v hcc || echo "hcc")

if [ "$HCC" = "hcc" ]; then
    echo "Error: HolyC compiler (hcc) not found"
    echo "Run install.sh first"
    exit 1
fi

echo "Compiling HolyC server with hcc..."

cd "$(dirname "$0")"

# Compile the HolyC server using hcc
# hcc compiles HolyC source to an executable
# Include paths for holyc-lib headers
$HCC server.HC -o server \
    -I/usr/local/include \
    -I/holyc-lang/src \
    -I/holyc-lang/src/holyc-lib \
    -DHCC_LINK_SQLITE3=1

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

echo "Server built successfully: ./server"
chmod +x server