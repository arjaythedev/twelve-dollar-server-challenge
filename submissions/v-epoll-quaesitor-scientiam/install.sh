#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the C toolchain and builds the pinned
# V compiler from source into /opt/twelve-v/v. It follows the Makefile's bootstrap: vc/v.c
# builds v1 through the compat script, v1 builds v2 from cmd/v, and v2 is installed. It stops
# there: the Makefile's last step rebuilds the compiler with itself, which ran a 2 GB box out
# of memory. v2 finds vlib from any directory, and build.sh uses it.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential git curl ca-certificates liburing-dev

V_COMMIT_FULL=b5bcdbb224752525c0a38aaccd3487a78641e269  # vlang/v master, 2026-10-23
# vlang/vc holds the generated bootstrap C (vc/v.c). Pinned to the newest vc commit that
# tracks a V commit which is an ancestor of V_COMMIT_FULL (its message says which one).
VC_COMMIT_FULL=7ba7d3e44009617d70c849764f89415b1a517690  # tracks V ffc38ada

rm -rf /opt/twelve-v
git init -q /opt/twelve-v
git -C /opt/twelve-v fetch -q --depth 1 https://github.com/vlang/v "$V_COMMIT_FULL"
git -C /opt/twelve-v checkout -q FETCH_HEAD
[ "$(git -C /opt/twelve-v rev-parse HEAD)" = "$V_COMMIT_FULL" ]

git init -q /opt/twelve-v/vc
git -C /opt/twelve-v/vc fetch -q --depth 1 https://github.com/vlang/vc "$VC_COMMIT_FULL"
git -C /opt/twelve-v/vc checkout -q FETCH_HEAD
[ "$(git -C /opt/twelve-v/vc rev-parse HEAD)" = "$VC_COMMIT_FULL" ]

cd /opt/twelve-v
sh cmd/tools/vc_compat.sh vc/v.c cc -std=gnu11 -w -DCUSTOM_DEFINE_v1_fallback -o v1 -- -lm -lpthread
./v1 -no-parallel -o v2 -gc none cmd/v
rm -f v1
mv v2 v
chmod -R a+rX /opt/twelve-v
/opt/twelve-v/v version
