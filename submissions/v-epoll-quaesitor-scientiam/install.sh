#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the C toolchain and builds the pinned
# V compiler from source into /opt/twelve-v.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential git curl ca-certificates liburing-dev

V_COMMIT_FULL=b5bcdbb224752525c0a38aaccd3487a78641e269  # vlang/v master, 2026-10-23

rm -rf /opt/twelve-v
git init -q /opt/twelve-v
git -C /opt/twelve-v fetch -q --depth 1 https://github.com/vlang/v "$V_COMMIT_FULL"
git -C /opt/twelve-v checkout -q FETCH_HEAD
cd /opt/twelve-v
make
# the build must have produced the pinned checkout, not a cached or updated one
[ "$(git rev-parse HEAD)" = "$V_COMMIT_FULL" ]
chmod -R a+rX /opt/twelve-v
/opt/twelve-v/v version
