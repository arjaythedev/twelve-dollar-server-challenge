#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler, and the Lean toolchain that checks
# the specification's proofs and generates the application layer (pinned, checksummed).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential curl ca-certificates zstd unzip >/dev/null

LEAN_VERSION=4.35.0
case "$(uname -m)" in
  x86_64)  asset=lean-$LEAN_VERSION-linux.tar.zst
           sha=7289597a44543246c9227b8cae501767e095ce17f6d448b95fc4a8dc97268f6d ;;
  aarch64) asset=lean-$LEAN_VERSION-linux_aarch64.tar.zst
           sha=c8f0f8a3920e839718b1343e6781fd80a3e9b1024fbcd7078eb30f54e806b028 ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac
dest=/opt/lean-$LEAN_VERSION
if [ ! -x "$dest/bin/lake" ]; then
  tmp="$(mktemp -d)"
  curl -fsSL --retry 3 -o "$tmp/$asset" "https://github.com/leanprover/lean4/releases/download/v$LEAN_VERSION/$asset"
  echo "$sha  $tmp/$asset" | sha256sum -c -
  mkdir -p "$dest"
  tar --zstd -xf "$tmp/$asset" -C "$dest" --strip-components=1
  rm -rf "$tmp"
fi
"$dest/bin/lean" --version
