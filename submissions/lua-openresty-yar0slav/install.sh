#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs a pinned OpenResty (nginx + LuaJIT) from the
# official OpenResty apt repository, plus Ubuntu's SQLite library (used from Lua through LuaJIT's FFI).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
OPENRESTY_VERSION=1.31.1.1-1~noble1

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gnupg libsqlite3-0

install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://openresty.org/package/pubkey.gpg | gpg --dearmor --yes -o /etc/apt/keyrings/openresty.gpg
case "$(dpkg --print-architecture)" in
  amd64) repo=https://openresty.org/package/ubuntu ;;
  arm64) repo=https://openresty.org/package/arm64/ubuntu ;;
  *) echo "unsupported architecture"; exit 1 ;;
esac
echo "deb [signed-by=/etc/apt/keyrings/openresty.gpg] $repo noble main" > /etc/apt/sources.list.d/openresty.list
apt-get update
apt-get install -y --no-install-recommends "openresty=$OPENRESTY_VERSION"
apt-mark hold openresty

# The package enables and starts a system-wide OpenResty on port 80; start.sh runs its own instead.
systemctl disable --now openresty 2>/dev/null || true
/usr/local/openresty/bin/openresty -v
