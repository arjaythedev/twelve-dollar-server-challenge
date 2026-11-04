#!/usr/bin/env bash
# Installs the AILANG compiler. AILANG has no apt package; its compiler is a single
# self-hosted binary published in the AILANG repository, pinned here by commit and sha256.
set -euo pipefail
REPO=https://github.com/AiLang-Author/Ailang-Self-Hosting-
COMMIT=2c027a8e0faf973cde2398f382910d605e2d5ffe
SHA256=67e25f5b47f77bb461f7b7aad5f719aaa2ea7ddc403a569fe9d3a7962cf65c37
DEST=/usr/local/bin/ailang.x
if ! command -v curl >/dev/null; then
  apt-get update -qq && apt-get install -y -qq curl ca-certificates
fi
tmp="$(mktemp)"
curl -fsSL -o "$tmp" "https://raw.githubusercontent.com/AiLang-Author/Ailang-Self-Hosting-/$COMMIT/ailang.x"
echo "$SHA256  $tmp" | sha256sum -c --quiet
install -m 0755 "$tmp" "$DEST"
rm -f "$tmp"
echo "installed $DEST from $REPO at $COMMIT"
