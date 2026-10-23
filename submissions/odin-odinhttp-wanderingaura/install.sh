#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the pinned Odin compiler and build prerequisites.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

ODIN_VERSION=dev-2026-09

apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl git clang unzip

rm -rf /usr/local/odin
mkdir -p /usr/local/odin
curl -fsSL "https://github.com/odin-lang/Odin/releases/download/${ODIN_VERSION}/odin-linux-amd64-${ODIN_VERSION}.tar.gz" \
    | tar -C /usr/local/odin --strip-components=1 -xz
ln -sf /usr/local/odin/odin /usr/local/bin/odin
odin version
