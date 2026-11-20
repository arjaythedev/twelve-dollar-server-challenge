#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: assembler, C compiler/linker
# (for the pinned SQLite amalgamation and libc), and download tools.
set -euo pipefail
[[ $(id -u) == 0 ]] || { echo 'install.sh must run as root' >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential nasm curl ca-certificates unzip
nasm -v
