#!/usr/bin/env bash
# install.sh: runs once as root on a clean Ubuntu 24.04.
# Installs the gforth toolchain and the C libraries the FFI layer links against.
#
# Versions are pinned to the Ubuntu 24.04 (Noble) archive — the distro release
# IS the pin, and nothing external is downloaded or built. The exact builds this
# was developed and tested against are:
#   gforth          0.7.3+dfsg-9build4.1
#   libsqlite3-dev  3.45.1-1ubuntu2.8   (SQLite 3.45.1)
#   libssl-dev      3.0.13-0ubuntu3.16  (OpenSSL 3.0.13)
#   libtool-bin     2.4.7-7build1
#   build-essential 12.10ubuntu1
# We intentionally let apt resolve the exact point build rather than hard-pinning
# the "-Nubuntu" string: Ubuntu security updates supersede and purge the older
# point build, so a hard `pkg=exact-version` could fail to install on the box.
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
# gforth           : the Forth system (provides the c-library FFI used for SQLite/crypto/sockets)
# build-essential  : cc, needed by gforth's c-library to compile FFI glue at load time
# libtool-bin      : provides the `libtool` *binary* gforth's c-library invokes
#                    (the plain `libtool` package ships only m4 macros, not the command)
# libsqlite3-dev   : provides -lsqlite3 (libsqlite3.so symlink) + sqlite3.h for the FFI link step
# libssl-dev       : provides -lcrypto (libcrypto.so symlink) for HMAC-SHA256
# sqlite3, ca-certs : convenience / TLS roots for any apt fetches
apt-get install -y --no-install-recommends \
  gforth \
  build-essential \
  libtool-bin \
  libsqlite3-dev \
  libssl-dev \
  sqlite3 \
  ca-certificates

# Show versions for the build log.
gforth --version 2>&1 | head -1 || true
echo "install.sh complete"
