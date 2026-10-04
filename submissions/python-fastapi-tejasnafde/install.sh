#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs Python 3.12 (Ubuntu's own package) and venv.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends python3 python3-venv
python3 --version
