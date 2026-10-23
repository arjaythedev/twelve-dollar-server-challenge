#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential python3 curl ca-certificates xz-utils
NODE_VERSION="24.21.0"
curl -fsSL "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" | tar -C /usr/local -xJ --strip-components=1
node --version
npm --version
