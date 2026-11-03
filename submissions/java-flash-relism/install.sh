#!/usr/bin/env bash
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl tar
archive="$(mktemp)"
trap 'rm -f "$archive"' EXIT
curl --fail --location --retry 3 \
    'https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.11%2B10/OpenJDK21U-jdk_x64_linux_hotspot_21.0.11_10.tar.gz' \
    -o "$archive"
echo "4b2220e232a97997b436ca6ab15cbf70171ecff52958a46159dfa5a8c44ca4de  $archive" | sha256sum -c -
mkdir -p /opt/flash-feed/jdk
tar -xzf "$archive" -C /opt/flash-feed/jdk --strip-components=1
