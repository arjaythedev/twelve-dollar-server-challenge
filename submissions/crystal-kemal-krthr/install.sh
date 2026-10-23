#!/usr/bin/env bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential ca-certificates curl git pkg-config \
  libevent-dev libgc-dev libpcre2-dev libsqlite3-dev libssl-dev zlib1g-dev

archive=$(mktemp)
trap 'rm -f "$archive"' EXIT
curl -fsSL https://github.com/crystal-lang/crystal/releases/download/1.21.1/crystal-1.21.1-1-linux-x86_64.tar.gz -o "$archive"
printf '%s  %s\n' 08b5779df8cf280c4a799599862bbe4d42103ba082aa73429562713c8a9f3b43 "$archive" | sha256sum --check
mkdir -p /opt/crystal-1.21.1
tar -xzf "$archive" -C /opt/crystal-1.21.1 --strip-components=1
ln -sfn /opt/crystal-1.21.1/bin/crystal /usr/local/bin/crystal
ln -sfn /opt/crystal-1.21.1/bin/shards /usr/local/bin/shards
