#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the .NET 10 SDK from Microsoft's official tarball.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
SDK_VERSION=10.0.401
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl libicu74
rm -rf /usr/local/dotnet && mkdir -p /usr/local/dotnet
curl -fsSL "https://builds.dotnet.microsoft.com/dotnet/Sdk/$SDK_VERSION/dotnet-sdk-$SDK_VERSION-linux-x64.tar.gz" \
  | tar -xz -C /usr/local/dotnet
ln -sf /usr/local/dotnet/dotnet /usr/local/bin/dotnet
dotnet --version
