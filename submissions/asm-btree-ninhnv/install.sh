#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: NASM, a C compiler/linker (for the SQLite
# amalgamation and libc), and curl/unzip to fetch SQLite.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential nasm curl ca-certificates unzip
nasm -v
