#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the C toolchain and the tools build.sh needs.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl unzip ca-certificates
