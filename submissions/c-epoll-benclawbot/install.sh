#!/usr/bin/env bash
set -euo pipefail
. /etc/os-release
if [[ "$ID" != "ubuntu" || "$VERSION_ID" != "24.04" ]]; then
  echo "This installer targets Ubuntu 24.04 x86_64, per challenge rule 8" >&2
  exit 1
fi
export DEBIAN_FRONTEND=noninteractive
apt-get update
# Pin the build tools and direct library dependencies to Ubuntu Noble releases.
# apt resolves transitive system dependencies from the same Ubuntu 24.04 repositories.
apt-get install -y --no-install-recommends \
  build-essential=12.10ubuntu1 \
  pkg-config=1.8.1-2build1 \
  libsqlite3-dev=3.45.1-1ubuntu2.8 \
  libjson-c-dev=0.17-1build1 \
  libssl-dev=3.0.13-0ubuntu3.16
