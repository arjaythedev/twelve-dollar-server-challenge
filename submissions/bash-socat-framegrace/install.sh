#!/usr/bin/env bash
# install.sh: runs once as root on a clean Ubuntu 24.04
set -euo pipefail

apt-get update && apt-get install -y socat sqlite3 openssl
