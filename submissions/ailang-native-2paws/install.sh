#!/usr/bin/env bash
# Installs the AILANG compiler. AILANG has no apt package; its compiler is a single
# self-hosted binary published in the AILANG repository, pinned here by commit and sha256.
set -euo pipefail
REPO=https://github.com/AiLang-Author/Ailang-Self-Hosting-
COMMIT=6ced82fa98637c80a490381f86578b2eff0ce3b8
SHA256=81c9077b6467176bb2e0ede107986ef8f789a8f2d51a8ffac1f2a32f2f54331f
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
