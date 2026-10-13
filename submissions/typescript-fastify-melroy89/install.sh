#!/usr/bin/env bash
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl xz-utils python3 make g++
archive="$(mktemp)"
trap 'rm -f "$archive"' EXIT
curl -fsSL https://nodejs.org/dist/v26.11.1/node-v26.11.1-linux-x64.tar.xz -o "$archive"
echo "3883bfc73f9a680ca4eab04b196068aaaab1373ffa77d8fc1a4408222495b651  $archive" | sha256sum -c -
tar -xJf "$archive" -C /usr/local --strip-components=1
