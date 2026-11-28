#!/usr/bin/env bash
# Installs the AILANG compiler. AILANG has no apt package; its compiler is a single
# self-hosted binary published in the AILANG repository, pinned here by commit and sha256.
set -euo pipefail
REPO=https://github.com/AiLang-Author/Ailang-Self-Hosting-
COMMIT=0117d392c54333c62366aac0a9585629982c4509
SHA256=6e4f069f809f9b2bb3491bf0fb5d9f55f479417574874d60d947db9115b66daa
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
