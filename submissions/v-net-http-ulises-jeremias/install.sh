#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl libsqlite3-dev pkg-config

# Pin V and its C backend so clean builds use the same compiler sources.
V_COMMIT=d67ee5500338554f54c7c8ca4cc00df42159577d
VC_COMMIT=bcfdc7f33e7b7908ee40f76d059c0e2894fb4a3b
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

curl -fsSL "https://github.com/vlang/v/archive/${V_COMMIT}.tar.gz" | tar -xz -C "$tmp"
mv "$tmp/v-${V_COMMIT}" /opt/vlang
curl -fsSL "https://github.com/vlang/vc/archive/${VC_COMMIT}.tar.gz" | tar -xz -C "$tmp"
mv "$tmp/vc-${VC_COMMIT}" /opt/vlang/vc
VJOBS=1 make -C /opt/vlang v
ln -sf /opt/vlang/v /usr/local/bin/v
v version
