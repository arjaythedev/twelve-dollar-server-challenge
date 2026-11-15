#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler, curl and git, and
# the Nexium compiler (nx) built from its source at one pinned commit.
#
# Nothing prebuilt is downloaded. The compiler is written in Nexium, and its
# repository carries a C seed (the C an earlier nx emitted for itself). The
# bootstrap needs only gcc:
#   gcc builds nx0 from the seed, nx0 builds the compiler's Nexium source into
#   nx1, nx1 rebuilds that source into nx2, and the script fails unless nx1 and
#   nx2 emit the same C for the compiler.
# It takes two to three minutes and about 600 MB of memory.
#
# The commit is fetched by its full hash, so git itself checks every byte of
# the source against the pin.
# Set NX_SOURCE to a local tarball of that commit to install offline (testing
# only: a tarball is not checked against the pin).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates git

NX_VER=1.5.0
NX_REPO=https://github.com/Londopy/nexium
NX_COMMIT=3224bf21716c5fe072481232b2bef0938bbc5912 # the v1.5.0 tag
NX_DIR="/opt/nexium-${NX_VER}"

if [ ! -x "${NX_DIR}/bin/nx" ]; then
  src="$(mktemp -d)"
  if [ -n "${NX_SOURCE:-}" ]; then
    tar -xzf "$NX_SOURCE" -C "$src" --strip-components=1
  else
    git -C "$src" init -q
    git -C "$src" fetch -q --depth 1 "$NX_REPO" "$NX_COMMIT"
    git -C "$src" -c advice.detachedHead=false checkout -q FETCH_HEAD
    [ "$(git -C "$src" rev-parse HEAD)" = "$NX_COMMIT" ]
  fi
  (cd "$src" && CC=gcc NX_CC=gcc NX_OFFLINE=1 NX_NO_UPDATE_CHECK=1 sh bootstrap/build.sh)
  install -D -m 0755 "$src/nx-out/bootstrap/nx2" "${NX_DIR}/bin/nx"
  install -D -m 0644 "$src/LICENSE" "${NX_DIR}/LICENSE"
fi
ln -sf "${NX_DIR}/bin/nx" /usr/local/bin/nx
NX_OFFLINE=1 NX_NO_UPDATE_CHECK=1 nx version
