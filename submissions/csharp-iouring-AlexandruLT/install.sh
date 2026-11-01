#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: clang/make/zlib for Native AOT and liburing,
# and the checksum-pinned .NET SDK.
set -euo pipefail
apt-get update
apt-get install -y ca-certificates curl clang make zlib1g-dev
version=10.0.300
mkdir -p /opt/twelve-dotnet
curl -fsSL "https://builds.dotnet.microsoft.com/dotnet/Sdk/$version/dotnet-sdk-$version-linux-x64.tar.gz" -o /tmp/twelve-dotnet.tar.gz
printf '%s  %s\n' a0c404c1a2f85d70e32392ce297eb388c0310c519521b538a031a895469444c67f347d4f9ca1f8441f525967a89c9b75e2cd1676da486f95118cf4025c38d904 /tmp/twelve-dotnet.tar.gz | sha512sum -c -
tar -xzf /tmp/twelve-dotnet.tar.gz -C /opt/twelve-dotnet
ln -sf /opt/twelve-dotnet/dotnet /usr/local/bin/dotnet
