#!/usr/bin/env bash
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl xz-utils python3 make g++
archive="$(mktemp)"
trap 'rm -f "$archive"' EXIT
curl -fsSL https://nodejs.org/dist/v24.15.0/node-v24.15.0-linux-x64.tar.xz -o "$archive"
echo "472655581fb851559730c48763e0c9d3bc25975c59d518003fc0849d3e4ba0f6  $archive" | sha256sum -c -
tar -xJf "$archive" -C /usr/local --strip-components=1
