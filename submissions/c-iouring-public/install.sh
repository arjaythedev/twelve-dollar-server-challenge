#!/usr/bin/env bash
# install.sh — runs once as root on a clean Ubuntu 24.04: the toolchain only (gcc 13, curl/unzip for the pinned
# SQLite download, python3 for the parser generator). No liburing: the event loop uses raw io_uring syscalls.
set -euo pipefail
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends build-essential curl unzip ca-certificates python3
