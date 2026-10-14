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
# V 0.5.2's V3 cache generator drops names from the duplicate fasthttp C union
# aliases. Rename the unused aliases without changing their C types.
sed -i 's/^\([[:space:]]*\)u32 u32$/\1u32_value u32/; s/^\([[:space:]]*\)u64 u64$/\1u64_value u64/' /opt/vlang/vlib/fasthttp/fasthttp_linux.c.v
# The challenge uses small headers and feed responses under 8 KiB. Keep the
# per-connection buffers small for that workload; fasthttp grows them as needed.
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
