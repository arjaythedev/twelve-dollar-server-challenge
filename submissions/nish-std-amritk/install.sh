#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: LLVM 18 from apt (clang-18 builds
# the binary, lld-18 links it), Node.js 22.18.0 from its official tarball
# (checked against the SHA-256 nodejs.org publishes for it), and the Nish
# compiler 0.49.0 from npm into /opt/nish, readable by every user.
#
# NISH_PACKAGES, when set, is what `npm install` installs instead of
# nish@0.49.0 (local tarballs from `npm pack`, to try an unreleased build).
set -euo pipefail
NODE_VERSION=22.18.0
NODE_SHA256=c1bfeecf1d7404fa74728f9db72e697decbd8119ccc6f5a294d795756dfcfca7
NISH_VERSION=0.49.0

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q clang-18 lld-18 llvm-18 curl ca-certificates xz-utils

if [ ! -x /opt/node/bin/node ] || [ "$(/opt/node/bin/node --version)" != "v$NODE_VERSION" ]; then
  tarball="node-v$NODE_VERSION-linux-x64.tar.xz"
  tmp="$(mktemp -d)"
  curl -fsSL -o "$tmp/$tarball" "https://nodejs.org/dist/v$NODE_VERSION/$tarball"
  echo "$NODE_SHA256  $tmp/$tarball" | sha256sum -c -
  rm -rf /opt/node
  mkdir -p /opt/node
  tar -xJf "$tmp/$tarball" -C /opt/node --strip-components=1
  rm -rf "$tmp"
fi

# shellcheck disable=SC2086 # NISH_PACKAGES is a list of words on purpose
PATH="/opt/node/bin:$PATH" npm install -g --prefix /opt/nish --no-audit --no-fund ${NISH_PACKAGES:-nish@$NISH_VERSION}
# Readable by every user and writable only by root, whatever umask npm ran
# under: the compiler refuses a package directory every user can write.
chmod -R a+rX,go-w /opt/node /opt/nish
/opt/nish/bin/nish --version
clang-18 --version | head -n 1
