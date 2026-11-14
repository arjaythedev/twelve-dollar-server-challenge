#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04
set -euo pipefail
apt-get update && apt-get install -y build-essential curl
curl -fsSL https://go.dev/dl/go1.27.1.linux-amd64.tar.gz | tar -C /usr/local -xz
