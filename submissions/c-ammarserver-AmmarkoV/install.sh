#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04.
#   build-essential cmake git  : AmmarServer is built from source with its own CMake
#   libssl-dev                 : libcrypto, for the HMAC-SHA256 behind every token verification
#   curl unzip ca-certificates : fetching the pinned SQLite amalgamation in build.sh
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    libssl-dev \
    curl \
    unzip \
    ca-certificates
