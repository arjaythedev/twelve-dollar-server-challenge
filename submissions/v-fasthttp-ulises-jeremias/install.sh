#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl git libsqlite3-dev pkg-config

# Pin both repositories: V's default Makefile otherwise pulls a moving vc branch.
V_COMMIT=d67ee5500338554f54c7c8ca4cc00df42159577d
VC_COMMIT=bcfdc7f33e7b7908ee40f76d059c0e2894fb4a3b
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL "https://github.com/vlang/v/archive/${V_COMMIT}.tar.gz" | tar -xz -C "$tmp"
mv "$tmp/v-${V_COMMIT}" /opt/vlang
# V 0.5.2's V3 module cache drops the field name when a C union member has
# the same name as its type. These unused aliases preserve the C ABI and let
# the generated cache parse correctly.
sed -i 's/^\([[:space:]]*\)u32 u32$/\1u32_value u32/; s/^\([[:space:]]*\)u64 u64$/\1u64_value u64/' /opt/vlang/vlib/fasthttp/fasthttp_linux.c.v
# The challenge's headers are small and the largest seeded feed JSON is about
# 5 KiB. fasthttp grows these buffers when needed, so smaller initial capacities
# reduce memory held by idle keep-alive connections.
fasthttp_linux=/opt/vlang/vlib/fasthttp/fasthttp_linux.v
grep -qFx 'const read_buf_cap = 8 * 1024' "$fasthttp_linux"
grep -qFx 'const write_buf_cap = 16 * 1024' "$fasthttp_linux"
sed -i 's/^const read_buf_cap = 8 \* 1024$/const read_buf_cap = 1024/; s/^const write_buf_cap = 16 \* 1024$/const write_buf_cap = 8 * 1024/' "$fasthttp_linux"
grep -qFx 'const read_buf_cap = 1024' "$fasthttp_linux"
grep -qFx 'const write_buf_cap = 8 * 1024' "$fasthttp_linux"
curl -fsSL "https://github.com/vlang/vc/archive/${VC_COMMIT}.tar.gz" | tar -xz -C "$tmp"
mv "$tmp/vc-${VC_COMMIT}" /opt/vlang/vc
VJOBS=1 make -C /opt/vlang v
ln -sf /opt/vlang/v /usr/local/bin/v
v version
