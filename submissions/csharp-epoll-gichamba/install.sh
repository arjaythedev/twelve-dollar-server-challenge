#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the pinned .NET 10 SDK from the official tarball,
# plus clang (compiles SQLite, links the Native AOT binary), binutils (ar, for the SQLite library)
# and zlib1g-dev (needed by the AOT link).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl clang binutils zlib1g-dev

SDK_VERSION=10.0.401
mkdir -p /usr/share/dotnet
curl -fsSL "https://builds.dotnet.microsoft.com/dotnet/Sdk/${SDK_VERSION}/dotnet-sdk-${SDK_VERSION}-linux-x64.tar.gz" \
  | tar -C /usr/share/dotnet -xz
ln -sf /usr/share/dotnet/dotnet /usr/local/bin/dotnet
dotnet --version
