#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler, and LuaJIT 2.1 built from its pinned,
# sha256-checked source into /usr/local (LuaJIT is a rolling release: the pin is a commit).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
LJ=63c8f1a9b479ba5518cf6a170ff7729178bc3cfd
LJ_SHA=8cad76f444b76001b7123dc626c5d0bfc24f1f790333737443345f7e6b41bb5f

apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl unzip

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
curl -fsSL --retry 3 -o "$TMP/luajit.tar.gz" "https://github.com/LuaJIT/LuaJIT/archive/$LJ.tar.gz"
echo "$LJ_SHA  $TMP/luajit.tar.gz" | sha256sum -c -
tar -C "$TMP" -xzf "$TMP/luajit.tar.gz"
make -C "$TMP/LuaJIT-$LJ" -j"$(nproc)" amalg PREFIX=/usr/local
make -C "$TMP/LuaJIT-$LJ" install PREFIX=/usr/local
ln -sf "$(ls /usr/local/bin/luajit-2.1.* | head -1)" /usr/local/bin/luajit
/usr/local/bin/luajit -v
