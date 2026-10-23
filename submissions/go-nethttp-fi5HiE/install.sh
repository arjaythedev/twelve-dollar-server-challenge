#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
GO=go1.27.1.linux-amd64.tar.gz
GO_SHA256=63d339f0da5ab53635a56f2490a7984dfe12dfcff22ad749f63edaf590168445
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl build-essential
tmp="$(mktemp -d)"
curl -fsSL "https://go.dev/dl/$GO" -o "$tmp/$GO"
echo "$GO_SHA256  $tmp/$GO" | sha256sum -c -
rm -rf /usr/local/go
tar -C /usr/local -xzf "$tmp/$GO"
rm -rf "$tmp"
/usr/local/go/bin/go version
