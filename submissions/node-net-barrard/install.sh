#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the official Node.js 24 LTS tarball to /usr/local.
set -euo pipefail
NODE_VERSION=v24.21.0
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl xz-utils
curl -fsSL "https://nodejs.org/dist/$NODE_VERSION/node-$NODE_VERSION-linux-x64.tar.xz" \
  | tar -xJ -C /usr/local --strip-components=1
node --version
