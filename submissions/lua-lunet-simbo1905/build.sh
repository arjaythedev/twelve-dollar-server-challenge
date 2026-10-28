#!/usr/bin/env bash
# This is an install of a pinned official release, not a source build. Run as a
# normal user; re-running is safe.
set -euo pipefail

LUNET_VERSION=v0.14.2
BASE_URL="https://github.com/lua-lunet/lunet/releases/download/$LUNET_VERSION"

case "$(uname -s)-$(uname -m)" in
    Darwin-*)            ASSET=lunet-macos.tar.gz ;      DIGEST=221d825cb16ad50593266e6f44ceccbeceaa2212bbd10cf45445fc02ec896bd1 ;;
    Linux-x86_64)        ASSET=lunet-linux-amd64.tar.gz; DIGEST=09955f35860a1a9fe4e4bda5269f782415cd1d60e6b8d7d1df0878464a9f685e ;;
    Linux-aarch64)       ASSET=lunet-linux-arm64.tar.gz; DIGEST=809863e44927e119f30662fc85c4a9ebc5ff950a8e231e657ebed42d2cf38c18 ;;
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
