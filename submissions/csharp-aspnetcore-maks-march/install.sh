#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the pinned .NET 10 SDK (LTS) from the
# official tarball. The SDK bundles the ASP.NET Core 10 shared runtime we publish against.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl libicu74

SDK_VERSION=10.0.401   # includes ASP.NET Core runtime 10.0.12
mkdir -p /usr/share/dotnet
curl -fsSL "https://builds.dotnet.microsoft.com/dotnet/Sdk/${SDK_VERSION}/dotnet-sdk-${SDK_VERSION}-linux-x64.tar.gz" \
  | tar -C /usr/share/dotnet -xz
ln -sf /usr/share/dotnet/dotnet /usr/local/bin/dotnet
dotnet --version
