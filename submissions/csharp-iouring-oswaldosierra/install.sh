#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the C toolchain (SQLite and the Native AOT link), ICU (the .NET SDK needs it)
# and the pinned .NET SDK, checked against the SHA-512 Microsoft publishes for it.
set -euo pipefail

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl build-essential binutils zlib1g-dev libicu74

SDK_VERSION=10.0.401
SDK_SHA512=51c8b999af9e8dd9998c9edc5944e19a90788862068acd38694e098889054ce8c23d4f0c5cccfa16bf187d044562359e5ee69a9f8ad0bbe913ba90311fbce25b
tmp="$(mktemp -d)"
curl -fsSL --retry 3 -o "$tmp/sdk.tar.gz" \
  "https://builds.dotnet.microsoft.com/dotnet/Sdk/${SDK_VERSION}/dotnet-sdk-${SDK_VERSION}-linux-x64.tar.gz"
echo "${SDK_SHA512}  $tmp/sdk.tar.gz" | sha512sum -c -
mkdir -p /usr/share/dotnet
tar -C /usr/share/dotnet -xzf "$tmp/sdk.tar.gz"
rm -rf "$tmp"
ln -sf /usr/share/dotnet/dotnet /usr/local/bin/dotnet
dotnet --version
