#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
NODE_VERSION=24.21.0
PNPM_VERSION=10.33.0

case "$(uname -m)" in
  x86_64) NODE_ARCH=x64 ;;
  aarch64) NODE_ARCH=arm64 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac
NODE_DIST="node-v${NODE_VERSION}-linux-${NODE_ARCH}"

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl xz-utils

work="$(mktemp -d)"
cd "$work"
curl -fsSLO "https://nodejs.org/dist/v${NODE_VERSION}/${NODE_DIST}.tar.xz"
curl -fsSLO "https://nodejs.org/dist/v${NODE_VERSION}/SHASUMS256.txt"
grep " ${NODE_DIST}.tar.xz\$" SHASUMS256.txt | sha256sum -c -
tar -xJf "${NODE_DIST}.tar.xz" -C /usr/local --strip-components=1
cd /
rm -rf "$work"

npm install -g "pnpm@${PNPM_VERSION}"
node --version
pnpm --version
