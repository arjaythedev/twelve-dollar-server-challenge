#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: NASM, a C compiler/linker (for the SQLite
# amalgamation and libc), and curl/unzip to fetch SQLite.
set -euo pipefail
. /etc/os-release
if [[ "$ID" != ubuntu || "$VERSION_ID" != 24.04 ]]; then
  echo "This installer targets Ubuntu 24.04 x86_64, per challenge rule 8" >&2
  exit 1
fi
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential=12.10ubuntu1 nasm=2.16.01-1build1 \
  curl=8.5.0-2ubuntu10.16 ca-certificates=20260816ubuntu0.24.04.2 \
  unzip=6.0-28ubuntu4.1
nasm -v
