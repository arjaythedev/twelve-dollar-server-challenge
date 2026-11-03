#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler plus the tools build.sh uses to fetch SQLite and liburing.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip
gcc --version | head -1
