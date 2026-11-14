#!/bin/bash
set -euo pipefail

# Install the bare minimum needed to build Python 3.15
apt-get update
apt-get install -y --no-install-recommends \
    software-properties-common \
    build-essential \
    curl \
    ca-certificates

# Add PPA with Python 3.15
add-apt-repository -y ppa:deadsnakes/ppa
apt-get update

# Install Python 3.15 itself and venv (without recommended packages)
apt-get install -y --no-install-recommends \
    python3.15 \
    python3.15-venv \
    python3.15-dev

# Install uv (fast Python package manager)
curl -LsSf https://astral.sh/uv/install.sh | sh

# Make uv available in PATH for all users
install -m 0755 /root/.local/bin/uv /usr/local/bin/uv