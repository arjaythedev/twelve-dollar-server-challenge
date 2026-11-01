#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ $(id -u) != 0 || $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
  echo 'Run install.sh as root on Ubuntu 24.04 x86_64.' >&2
  exit 1
fi
source /etc/os-release
if [[ $ID != ubuntu || $VERSION_ID != 24.04 ]]; then
  echo 'Ubuntu 24.04 is required.' >&2
  exit 1
fi
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential gcc-13 ca-certificates curl unzip

temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
goose_dir=/opt/goose-v2026.3
if [[ -e $goose_dir || -L $goose_dir ]]; then
  if [[ ! -x $goose_dir/goose ]] || ! "$goose_dir/goose" version | grep -Fq 4d3da949e563; then
    echo "An incompatible installation already exists at $goose_dir." >&2
    exit 1
  fi
else
  curl -fsSL --retry 3 --max-time 180 \
    https://github.com/aardappel/goose/releases/download/v2026.3/goose-v2026.3-linux-x64.tar.gz \
    -o "$temporary/goose.tar.gz"
  echo "06ac30284338269d3996d457412bf4f2b47c1f9a5601de65b6317187fe78d5c9  $temporary/goose.tar.gz" | sha256sum -c -
  tar -xzf "$temporary/goose.tar.gz" -C "$temporary"
  mv "$temporary/goose-v2026.3-linux-x64" "$goose_dir"
fi
"$goose_dir/goose" version
