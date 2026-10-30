#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the C toolchain and the pinned V 0.5.2
# release into /opt/twelve-v, where build.sh looks for the compiler. The release archive ships
# a prebuilt compiler, so nothing is compiled here: building from source ran past the one-hour
# install timeout on a 1 vCPU box.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates liburing-dev unzip

V_RELEASE=0.5.2  # vlang/v release tag
V_ZIP_SHA256=86caf9e70c3342d48ef19eb4f6c47b709f18c90ae86255520d5c29df6b482e23  # v_linux.zip

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/v_linux.zip" "https://github.com/vlang/v/releases/download/$V_RELEASE/v_linux.zip"
echo "$V_ZIP_SHA256  $tmp/v_linux.zip" | sha256sum -c -
unzip -q "$tmp/v_linux.zip" -d "$tmp/extract"

rm -rf /opt/twelve-v
mv "$tmp/extract/v" /opt/twelve-v
chmod -R a+rX /opt/twelve-v
chmod +x /opt/twelve-v/v
/opt/twelve-v/v version
