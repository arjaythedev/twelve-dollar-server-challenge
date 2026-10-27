#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the Crystal 1.22.1 toolchain (official tarball,
# checksum-pinned) plus the C toolchain and headers it links against.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential pkg-config curl ca-certificates unzip libssl-dev libpcre2-dev libgc-dev zlib1g-dev libyaml-dev libgmp-dev >/dev/null
V=1.22.1
SHA=a4c4b7060bd7f44a17017442bf833e8e0638a495e9f6eec3781d9c63a0c1a128
if [ ! -x /opt/crystal-$V/bin/crystal ]; then
  curl -fsSL -o /tmp/crystal.tar.gz "https://github.com/crystal-lang/crystal/releases/download/$V/crystal-$V-1-linux-x86_64.tar.gz"
  echo "$SHA  /tmp/crystal.tar.gz" | sha256sum -c -
  mkdir -p /opt/crystal-$V && tar -C /opt/crystal-$V --strip-components=1 -xzf /tmp/crystal.tar.gz && rm /tmp/crystal.tar.gz
fi
ln -sf /opt/crystal-$V/bin/crystal /usr/local/bin/crystal
ln -sf /opt/crystal-$V/bin/shards /usr/local/bin/shards
crystal --version
