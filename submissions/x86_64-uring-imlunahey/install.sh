#!/usr/bin/env bash
set -euo pipefail
[[ $(id -u) == 0 ]] || { echo 'install.sh must run as root' >&2; exit 1; }
apt-get update
apt-get install -y --no-install-recommends build-essential nasm=2.16.01-1build1 curl ca-certificates python3 unzip
