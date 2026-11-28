#!/usr/bin/env bash
# install.sh — runs once as root on clean Ubuntu 24.04
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential \
  cmake \
  git \
  curl \
  unzip \
  ca-certificates \
  libssl-dev

CIDER_DIR=/opt/cider-v4
CIDER_COMMIT=e54015ec83aa2b5fb814ed0e8ecac0c942688fb5

if [ ! -x "$CIDER_DIR/build/cider" ]; then
  echo "== [Cider] Fetching and building Cider compiler from source..."
  rm -rf "$CIDER_DIR"
  git clone https://github.com/Pomilon/Cider.git "$CIDER_DIR"
  git -C "$CIDER_DIR" checkout -q "$CIDER_COMMIT"
  cmake -S "$CIDER_DIR" -B "$CIDER_DIR/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$CIDER_DIR/build" -j"$(nproc)"
fi

echo "== [Cider] Compiler built successfully at $CIDER_DIR/build/cider"
