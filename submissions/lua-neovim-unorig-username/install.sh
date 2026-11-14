#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs Neovim (the web server) and the SQLite
# shared library it loads through LuaJIT's FFI (3.45.1, the noble build; left unpinned so a security update cannot break the install). libcrypto (libssl3t64) is part of the base system.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends neovim=0.9.5-6ubuntu2 libsqlite3-0
nvim --version | head -2
