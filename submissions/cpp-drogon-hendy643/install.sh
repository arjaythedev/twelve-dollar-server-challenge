#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends clang-20 lld-20 cmake ninja-build git ca-certificates \
  libstdc++-14-dev libssl-dev libsqlite3-dev libgtest-dev zlib1g-dev uuid-dev libjsoncpp-dev
clang++-20 --version
