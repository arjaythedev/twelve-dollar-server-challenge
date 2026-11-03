#!/usr/bin/env bash
set -euo pipefail
RUBY_VERSION=4.0.5
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y build-essential curl openssl libssl-dev libyaml-dev zlib1g-dev libffi-dev libgmp-dev rustc tzdata
cd "$(mktemp -d)"
curl -fsSL "https://cache.ruby-lang.org/pub/ruby/${RUBY_VERSION%.*}/ruby-${RUBY_VERSION}.tar.gz" | tar -xz
cd "ruby-${RUBY_VERSION}"
./configure --prefix=/opt/ruby --enable-yjit --disable-install-doc
make -j"$(nproc)"
make install
