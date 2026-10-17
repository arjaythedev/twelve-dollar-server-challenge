#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: assembler, C compiler (for SQLite only), downloader.
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends build-essential nasm curl unzip ca-certificates
