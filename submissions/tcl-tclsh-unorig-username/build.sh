#!/usr/bin/env bash
# 1. Builds Tcl 8.7a5 from the release tarball into ./tcl. Its Linux notifier is epoll(7), so one process
#    can hold tens of thousands of sockets; Ubuntu's Tcl 8.6 waits with select() and stops at 1024.
#    tcl-epoll-fd-table.patch finds a socket's handler by its fd: 8.7a5 walks a list of every
#    handler each time a channel changes what it waits for, which is several times per request.
# 2. Compiles tcllib 1.21's sha256c accelerator (its C SHA-256, shipped as source in the tcllib
#    package) with critcl into ./lib, then checks that server.tcl at least parses.
set -euo pipefail
cd "$(dirname "$0")"
V=8.7a5; SUM=8a01858a8e8ab812faa7aa9af8dcc1c01891be9d69554b0178a4236ba1c0a1f0
rm -rf tcl src; mkdir -p src
curl -fsSL -o src/tcl.tar.gz "https://prdownloads.sourceforge.net/tcl/tcl$V-src.tar.gz"
echo "$SUM  src/tcl.tar.gz" | sha256sum -c -
tar -C src -xzf src/tcl.tar.gz
patch -d src/tcl$V -p1 < tcl-epoll-fd-table.patch
(cd src/tcl$V/unix && ./configure --prefix="$PWD/../../../tcl" --disable-shared >/dev/null)
make -C src/tcl$V/unix -j"$(nproc)" >/dev/null
make -C src/tcl$V/unix install-binaries install-libraries >/dev/null
rm -rf src
grep -q epoll_wait tcl/bin/tclsh8.7 || { echo "tclsh8.7 was built without the epoll notifier" >&2; exit 1; }
rm -rf lib .critcl
critcl -cache "$PWD/.critcl" -libdir "$PWD/lib" -pkg sha256c /usr/share/tcltk/tcllib1.21/sha1/sha256c.tcl
echo 'if {![info complete [read [open server.tcl]]]} {exit 1}' | tcl/bin/tclsh8.7
