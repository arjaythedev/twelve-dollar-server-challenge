#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the .NET 11 SDK (11.0.100) from Microsoft's official tarball (pinned,
# SHA-512 checked) into /usr/local/dotnet, plus gcc, unzip and curl (build.sh compiles SQLite's amalgamation) and the
# libraries the runtime loads (ICU for the SDK's build tools, OpenSSL for HMAC-SHA256).
set -euo pipefail
SDK=11.0.100
SHA512=1bec9cca2ec689b1e80245b84f2b53c9441d1aa6daccf2ceb27490768bffa95954fa585bd275def31069d874d543d9ba9e6591fe9ade2dbd9f34ec5c0fd0050c
DEST=/usr/local/dotnet
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends ca-certificates curl unzip gcc libc6-dev libicu74 libssl3t64 zlib1g
if [ -x "$DEST/dotnet" ] && "$DEST/dotnet" --list-sdks | grep -q "^$SDK "; then echo ".NET SDK $SDK already installed"; exit 0; fi
TMP="$(mktemp -d)"
curl -fsSL --retry 3 -o "$TMP/sdk.tar.gz" "https://builds.dotnet.microsoft.com/dotnet/Sdk/$SDK/dotnet-sdk-$SDK-linux-x64.tar.gz"
echo "$SHA512  $TMP/sdk.tar.gz" | sha512sum -c -
mkdir -p "$DEST"
tar -xzf "$TMP/sdk.tar.gz" -C "$DEST"
rm -rf "$TMP"
"$DEST/dotnet" --list-sdks
