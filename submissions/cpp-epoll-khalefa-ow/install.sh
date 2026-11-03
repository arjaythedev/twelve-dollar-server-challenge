#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C/C++ toolchain, OpenSSL's libcrypto (SHA-256) and curl.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential libssl-dev curl ca-certificates
g++ --version | head -1
