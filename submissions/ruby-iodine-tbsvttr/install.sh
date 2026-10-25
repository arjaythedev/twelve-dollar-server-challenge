#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates \
  libssl-dev libyaml-dev zlib1g-dev libffi-dev libgmp-dev rustc tzdata
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
curl -fsSL --retry 3 https://cache.ruby-lang.org/pub/ruby/4.0/ruby-4.0.5.tar.gz -o ruby.tar.gz
echo '7d6149079a63f8ae1d326c9fa65c6019ba2dc3155eae7b39159817911c88958e  ruby.tar.gz' | sha256sum -c -
tar -xzf ruby.tar.gz
cd ruby-4.0.5
./configure --prefix=/opt/ruby --enable-yjit --disable-install-doc
make -j"$(nproc)"
make install
/opt/ruby/bin/ruby --yjit -e 'abort "YJIT unavailable" unless RubyVM::YJIT.enabled?'
