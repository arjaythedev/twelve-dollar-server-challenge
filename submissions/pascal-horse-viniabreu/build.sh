#!/usr/bin/env bash
# Builds as the unprivileged submission user.
set -euo pipefail
cd "$(dirname "$0")"

HORSE_VERSION=3.3.14
HORSE_COMMIT=84194a3683b3973f64e28835566f02468cb6e916

rm -rf build bin modules
mkdir -p build/units bin modules
git clone --depth 1 --branch "$HORSE_VERSION" https://github.com/HashLoad/horse.git modules/horse
test "$(git -C modules/horse rev-parse HEAD)" = "$HORSE_COMMIT"
git -C modules/horse apply --whitespace=nowarn "$PWD/horse-epoll-timeout.patch"

fpc \
  -Mdelphi -O3 -OoREGVAR -Xs -XX -CX \
  -dHORSE_PROVIDER_EPOLL \
  -Fu./modules/horse/src \
  -FU./build/units -FE./bin \
  ./src/server.lpr
