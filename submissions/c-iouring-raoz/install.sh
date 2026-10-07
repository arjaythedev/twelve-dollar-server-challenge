#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C toolchain plus curl to fetch the pinned sources.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates
gcc --version | head -1
