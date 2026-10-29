#!/usr/bin/env bash
# Runs as a normal user. Builds the dex compiler from a pinned source revision,
# then compiles server.dx with it.
set -euo pipefail
cd "$(dirname "$0")"

# The compiler revision this submission was written and tested against.
DEX_VERSION="${DEX_VERSION:-v0.9.22}"

# DEX_BIN points at an already-built compiler, which is what the local test
# loop uses. CI and the benchmark box leave it unset and build from the pinned
# revision below.
if [ -n "${DEX_BIN:-}" ]; then
  DEX="$DEX_BIN"
else
  export PATH="/usr/local/go/bin:$PATH"
  export GOBIN="$PWD/.bin"
  # GOPATH and GOCACHE are left at their defaults on purpose: pinning them
  # inside this folder makes Go re-download the toolchain and the module cache
  # for every submission built on the same box, which is several minutes of
  # nothing. Only the binary lands here.

  # Resolved through the module proxy, so the revision is checksum-verified
  # against the Go checksum database rather than taken on trust from a clone.
  go install "github.com/ensamuel7/dex@${DEX_VERSION}"
  DEX="$GOBIN/dex"
fi

# dex emits C and then invokes cc on it, so the build needs the C toolchain
# install.sh put there, not just Go.
"$DEX" version
"$DEX" mod download
"$DEX" build server.dx

test -x build/server
echo "built build/server"
