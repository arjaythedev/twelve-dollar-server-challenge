#!/usr/bin/env bash
# install.sh: runs once as root on a clean Ubuntu 24.04. Installs the pinned Ecko release
# (one static binary with SQLite bundled) from the official download host.
set -euo pipefail
apt-get update && apt-get install -y curl ca-certificates
curl -fsSL https://ecko.sh/install | ECKO_VERSION=v0.80.0 ECKO_INSTALL_DIR=/usr/local/bin sh
