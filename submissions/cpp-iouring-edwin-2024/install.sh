#!/usr/bin/env bash
set -euo pipefail

echo "== Installing dependencies for C++23 io_uring server"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential \
    g++ \
    liburing-dev \
    curl \
    unzip \
    ca-certificates

echo "== Dependencies installed successfully"
