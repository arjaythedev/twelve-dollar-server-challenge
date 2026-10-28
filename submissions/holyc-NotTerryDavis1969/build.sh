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

# hcc does not accept GCC-style -I include flags; pass only the source and
# any supported compiler options. The local headers are included via relative
# path from the submission directory itself.
$HCC server.HC -o server -DHCC_LINK_SQLITE3=1 -I/usr/include

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

echo "Server built successfully: ./server"
chmod +x server