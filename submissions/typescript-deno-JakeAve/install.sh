#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs a pinned Deno into /usr/local/bin, and rustc for poller.rs.
set -euo pipefail
apt-get update && apt-get install -y curl unzip ca-certificates build-essential rustc
curl -fsSL https://deno.land/install.sh | DENO_INSTALL=/usr/local sh -s v2.9.7
/usr/local/bin/deno --version
