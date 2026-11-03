#!/usr/bin/env bash
# install.sh — run once as root on a clean Ubuntu 24.04.
# Toolchain only: gcc 13, libc headers, make, curl (build.sh fetches the
# SQLite amalgamation) and liburing 2.5. Versions are pinned to the Ubuntu
# 24.04 (noble) archive; if the archive has moved on to a newer point
# release, fall back to whatever noble ships for the same package.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

GCC_VER=13.3.0-6ubuntu2~24.04.1
URING_VER=2.5-1build1

apt-get update
if ! apt-get install -y --no-install-recommends \
      "gcc-13=$GCC_VER" "cpp-13=$GCC_VER" "liburing-dev=$URING_VER" "liburing2=$URING_VER" \
      gcc libc6-dev make curl ca-certificates; then
  echo "install.sh: pinned versions unavailable, installing noble's current ones" >&2
  apt-get install -y --no-install-recommends gcc-13 gcc libc6-dev make curl ca-certificates liburing-dev
fi
gcc --version | head -1
dpkg-query -W -f='liburing-dev ${Version}\n' liburing-dev
