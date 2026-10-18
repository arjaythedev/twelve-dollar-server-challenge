#!/usr/bin/env bash
# Fetches the pinned lunet v0.11.1 release archive and stages it into .lunet/.
# This is an install of a pinned official release, not a source build. Run as a
# normal user; re-running is safe.
set -euo pipefail

LUNET_VERSION=v0.11.1
BASE_URL="https://github.com/lua-lunet/lunet/releases/download/$LUNET_VERSION"

case "$(uname -s)-$(uname -m)" in
    Darwin-*)            ASSET=lunet-macos.tar.gz ;      DIGEST=c2b276be5c395b369b9006b03ce0461d3a8ee7f38e6cc98ff2da412bd6f6f747 ;;
    Linux-x86_64)        ASSET=lunet-linux-amd64.tar.gz; DIGEST=ca24cebf33a010198cf0af2e381c516f5995ad6a62e75bbee91b6a421a137150 ;;
    Linux-aarch64)       ASSET=lunet-linux-arm64.tar.gz; DIGEST=7b3d5662f1210c6ab0dc4b281f7e829f340c785b683ef27bf738a7661a0f39e9 ;;
    *) echo "build.sh: unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

URL="$BASE_URL/$ASSET"
ARCHIVE="$(mktemp -d)/$ASSET"

cd "$(dirname "$0")"

echo "== fetching $URL"
curl -fsSL -o "$ARCHIVE" "$URL"

echo "== verifying sha256"
actual="$(sha256sum "$ARCHIVE" 2>/dev/null || shasum -a 256 "$ARCHIVE")"
actual="${actual%% *}"
[ "$actual" = "$DIGEST" ] || { echo "sha256 mismatch: want $DIGEST got $actual" >&2; exit 1; }
echo "   ok"

echo "== staging .lunet/"
rm -rf .lunet
mkdir .lunet
tar -xzf "$ARCHIVE" -C .lunet

# smoke test
echo 'require("lunet"); require("lunet.socket"); require("lunet.sqlite3"); require("lunet.jsonic"); print("ok")' > .lunet/.smoke.lua
./.lunet/lunet-run .lunet/.smoke.lua
rm .lunet/.smoke.lua

echo "build.sh: $LUNET_VERSION staged in .lunet/ from $ASSET"
