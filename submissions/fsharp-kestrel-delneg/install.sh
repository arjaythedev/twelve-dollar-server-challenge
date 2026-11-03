#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the .NET 10 SDK (official tarball, pinned, SHA-512 checked),
# libicu (the SDK needs it) and gcc to compile SQLite from source.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gcc libc6-dev libicu74
SDK=10.0.401
case "$(uname -m)" in
  x86_64)  ARCH=x64;   SHA512=51c8b999af9e8dd9998c9edc5944e19a90788862068acd38694e098889054ce8c23d4f0c5cccfa16bf187d044562359e5ee69a9f8ad0bbe913ba90311fbce25b ;;
  aarch64) ARCH=arm64; SHA512=58ace73ced6b4360754689a686bdfb8a317f4da6cb8bb416dbc7d0ba9f47e43e3c09f5eb1f1a1cfaacbd10df9558da4882bf2a5e195d6ab56a02c1f9f76102ed ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac
curl -fsSL -o /tmp/dotnet-sdk.tar.gz "https://builds.dotnet.microsoft.com/dotnet/Sdk/$SDK/dotnet-sdk-$SDK-linux-$ARCH.tar.gz"
echo "$SHA512  /tmp/dotnet-sdk.tar.gz" | sha512sum -c -
mkdir -p /usr/share/dotnet
tar -xzf /tmp/dotnet-sdk.tar.gz -C /usr/share/dotnet
rm /tmp/dotnet-sdk.tar.gz
ln -sf /usr/share/dotnet/dotnet /usr/local/bin/dotnet
DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 dotnet --list-sdks
