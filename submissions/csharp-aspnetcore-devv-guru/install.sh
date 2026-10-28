#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
SDK_VERSION=10.0.401
SDK_SHA512=51c8b999af9e8dd9998c9edc5944e19a90788862068acd38694e098889054ce8c23d4f0c5cccfa16bf187d044562359e5ee69a9f8ad0bbe913ba90311fbce25b
SDK_ROOT="/opt/twelve-dollar-dotnet/$SDK_VERSION"
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl libicu74 libssl3t64 zlib1g
if [ ! -x "$SDK_ROOT/dotnet" ]; then
  archive="$(mktemp)"
  trap 'rm -f "$archive"' EXIT
  curl -fsSL --retry 3 "https://builds.dotnet.microsoft.com/dotnet/Sdk/$SDK_VERSION/dotnet-sdk-$SDK_VERSION-linux-x64.tar.gz" -o "$archive"
  printf '%s  %s\n' "$SDK_SHA512" "$archive" | sha512sum --check --status
  mkdir -p "$SDK_ROOT"
  tar -xzf "$archive" -C "$SDK_ROOT"
fi
"$SDK_ROOT/dotnet" --version
