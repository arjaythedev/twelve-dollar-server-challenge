#!/usr/bin/env bash
set -euo pipefail
# Run as root on Ubuntu 24.04. Install the exact SDK pinned by global.json.
apt-get update
apt-get install -y ca-certificates curl libc6 libgcc-s1 libgssapi-krb5-2 libicu74 libssl3t64 libstdc++6 zlib1g
installer="$(mktemp)"
trap 'rm -f "$installer"' EXIT
curl -fsSL https://dot.net/v1/dotnet-install.sh -o "$installer"
bash "$installer" --version 10.0.401 --install-dir /usr/local/share/dotnet --no-path
ln -sf /usr/local/share/dotnet/dotnet /usr/local/bin/dotnet
