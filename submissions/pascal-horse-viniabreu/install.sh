#!/usr/bin/env bash
# Runs once as root on Ubuntu 24.04.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
  build-essential \
  ca-certificates \
  git \
  fp-compiler \
  fp-units-base \
  fp-units-db \
  fp-units-fcl \
  libsqlite3-dev

fpc -iV
