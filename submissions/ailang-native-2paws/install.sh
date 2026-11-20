#!/usr/bin/env bash
# Installs the AILANG compiler. AILANG has no apt package; its compiler is a single
# self-hosted binary published in the AILANG repository, pinned here by commit and sha256.
set -euo pipefail
REPO=https://github.com/AiLang-Author/Ailang-Self-Hosting-
COMMIT=ed228e9e8f5d2c1bdccd963bcc6be9295bdff1fc
SHA256=7cc4128adaf3c1be31de15007aca1f4d4e187187026a6618c509d2f9c50f2ec4
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
