#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs gcc, liburing-dev, build-essential, curl, unzip
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends gcc liburing-dev build-essential curl unzip

DIR="$(cd "$(dirname "$0")" && pwd)"
if [ ! -f "$DIR/sqlite3.c" ] || [ ! -f "$DIR/sqlite3.h" ]; then
    echo "Downloading SQLite amalgamation..."
    TMP_ZIP="/tmp/sqlite_amalgamation_$$.zip"
    curl -fsSL "https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip" -o "$TMP_ZIP"
    unzip -q -o -j "$TMP_ZIP" -d "$DIR"
    rm -f "$TMP_ZIP"
fi
