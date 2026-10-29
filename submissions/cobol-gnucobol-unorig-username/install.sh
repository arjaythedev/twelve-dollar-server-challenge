#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: builds GnuCOBOL 3.2 from the official GNU release
# (SHA-256 pinned), plus the C toolchain, GMP and libcrypto that it and the server link against.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip xz-utils libgmp-dev libssl-dev
GC=gnucobol-3.2
SHA=3bb48af46ced4779facf41fdc2ee60e4ccb86eaa99d010b36685315df39c2ee2
tmp=$(mktemp -d)
curl -fsSL -o "$tmp/$GC.tar.xz" https://ftp.gnu.org/gnu/gnucobol/$GC.tar.xz
echo "$SHA  $tmp/$GC.tar.xz" | sha256sum -c -
tar -C "$tmp" -xf "$tmp/$GC.tar.xz"
cd "$tmp/$GC"
./configure --prefix=/usr/local --without-db --without-curses --without-xml2 --without-json --disable-nls CFLAGS=-O2
make -j"$(nproc)"
make install
ldconfig
cd /
rm -rf "$tmp"
cobc --version | head -1
