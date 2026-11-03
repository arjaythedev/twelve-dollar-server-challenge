#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: builds the pinned V compiler (0.5.2) into /opt/v from its pinned C bootstrap (about 3 minutes on 1 vCPU).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential git curl ca-certificates libssl-dev

V_COMMIT=7647ce1c6fad63b5578bc07883139906de74b2f8   # vlang/v tag 0.5.2
VC_COMMIT=7eb8c54a3843e5107d5af06d7a8c3e928f322475  # vlang/vc: C bootstrap of V 0.5.2
fetch() { # fetch <repo> <commit> <dir>
  git init -q "$3" && git -C "$3" fetch -q --depth 1 "$1" "$2" && git -C "$3" checkout -q FETCH_HEAD
}
rm -rf /opt/v
fetch https://github.com/vlang/v "$V_COMMIT" /opt/v
fetch https://github.com/vlang/vc "$VC_COMMIT" /opt/v/vc
cd /opt/v
# vc/v.c is already the full 0.5.2 compiler, so no v1/v2 self-rebuild (that took over an hour on 1 vCPU).
cc -std=gnu11 -w -O2 -o v vc/v.c -lm -lpthread
chmod -R a+rX /opt/v
/opt/v/v version
