#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: LuaTeX (the web server), the Lua 5.3 headers it was built with,
# a C compiler and SQLite's headers for the binding, and the binding's source (lsqlite3 0.9.6, SHA-256 pinned).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
# libsqlite3-dev is left unpinned (3.45.1, the noble build) so a security update cannot break the install.
apt-get install -y --no-install-recommends texlive-binaries=2023.20230311.66589-9build3 \
  libtexlua-dev=2023.20230311.66589-9build3 gcc libc6-dev libsqlite3-dev curl ca-certificates unzip
zip=$(mktemp)
curl -fsSL -o "$zip" 'http://lua.sqlite.org/home/zip/lsqlite3_v096.zip?uuid=v0.9.6'
echo "ecc6e7636a54f021bca5b4a01b35af06fd7a6fc8b21c4b3eccd4fdb5dd32ad82  $zip" | sha256sum -c -
rm -rf /usr/local/src/lsqlite3_v096
unzip -q "$zip" 'lsqlite3_v096/lsqlite3.c' -d /usr/local/src
rm -f "$zip"
luatex --version | head -1
