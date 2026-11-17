#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs Neovim (the web server) and the SQLite shared library it loads
# through LuaJIT's FFI. libcrypto (libssl3t64) is part of the base system.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends neovim=0.9.5-6ubuntu2
# SQLite: exactly libsqlite3-0 3.45.1-1ubuntu2, the noble release build. noble-updates and noble-security only ever
# list their latest build (3.45.1-1ubuntu2.9 and .8 today), so a pin to one of those breaks on the next update, and
# apt-get install of any other version than the newest fails where a newer one is already installed (CI installs
# sqlite3 first). So the package is downloaded and unpacked to /opt, apart from the system's copy.
d=$(mktemp -d) && cd "$d"
apt-get download -o APT::Sandbox::User=root libsqlite3-0=3.45.1-1ubuntu2
dpkg-deb -x libsqlite3-0_3.45.1-1ubuntu2_*.deb .
install -D -m 644 usr/lib/*/libsqlite3.so.0.8.6 /opt/lua-neovim-unorig-username/libsqlite3.so.0
cd / && rm -rf "$d"
nvim --version | head -2
