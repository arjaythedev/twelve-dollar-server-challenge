#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Tcl 8.6, the SQLite Tcl binding, tcllib, and what
# build.sh needs to compile tcllib's own C SHA-256 accelerator (critcl, gcc, the Tcl headers).
# libsqlite3-tcl is the package from the noble release pocket; it loads the system libsqlite3.so.0.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  tcl8.6=8.6.14+dfsg-1build1 tcl8.6-dev=8.6.14+dfsg-1build1 \
  libsqlite3-tcl=3.45.1-1ubuntu2 tcllib=1.21+dfsg-1 critcl=3.1.18.1+dfsg-3 gcc libc6-dev
