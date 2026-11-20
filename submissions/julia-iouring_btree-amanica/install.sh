#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: C toolchain (SQLite is compiled from source) and Julia.
set -euo pipefail
cd "$(dirname "$0")"
. ./config.sh
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential ca-certificates curl unzip openssl libssl3t64
if [ ! -x "$JULIA_HOME/bin/julia" ]; then
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  curl -fsSL -o "$tmp/julia.tar.gz" "$JULIA_URL"
  echo "$JULIA_SHA256  $tmp/julia.tar.gz" | sha256sum -c -
  mkdir -p "$JULIA_HOME"
  tar -C "$JULIA_HOME" --strip-components=1 -xzf "$tmp/julia.tar.gz"
fi
"$JULIA_HOME/bin/julia" --version
