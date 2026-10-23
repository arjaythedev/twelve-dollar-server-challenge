#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C/C++ toolchain (uWebSockets.js and better-sqlite3 are
# compiled from source by build.sh) and Node.js 24 LTS from the official tarball, in its own directory
# (/opt/node-v24.21.0) so it can't mix with any other Node on the box. build.sh and start.sh use it by path.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
NODE_VERSION=v24.21.0

apt-get update
apt-get install -y --no-install-recommends build-essential python3 git curl ca-certificates xz-utils

case "$(uname -m)" in
  x86_64)  arch=x64;   sha=fd8e59d5a511510f6a298afb548f18c7d2b1be404d8b4a27d94fbe49f56cb2d6 ;;
  aarch64) arch=arm64; sha=6ad1325edbdb5649c379b75a237147a666c95d4f9ae8d340fef2d1575d289ad2 ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac
tarball="node-$NODE_VERSION-linux-$arch.tar.xz"
tmp="$(mktemp -d)"
curl -fsSL "https://nodejs.org/dist/$NODE_VERSION/$tarball" -o "$tmp/$tarball"
echo "$sha  $tmp/$tarball" | sha256sum -c -
rm -rf "/opt/node-$NODE_VERSION" && mkdir -p "/opt/node-$NODE_VERSION"
tar -xJf "$tmp/$tarball" -C "/opt/node-$NODE_VERSION" --strip-components=1 --no-same-owner
rm -rf "$tmp"
"/opt/node-$NODE_VERSION/bin/node" --version
