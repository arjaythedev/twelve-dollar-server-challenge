#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the C toolchain and builds the pinned
# V compiler into /opt/twelve-v/v. The source comes straight from upstream: vlang/v at the
# pinned commit, and the vc bootstrap at its pinned commit. Each commit is fetched by SHA and
# checked against HEAD before anything is built. The bootstrap follows the Makefile: vc/v.c
# builds v1 through the compat script, v1 builds v2 from cmd/v, and v2 is installed. It stops
# there: the Makefile's last step rebuilds the compiler with itself, which ran a 2 GB box out
# of memory. v2 finds vlib from any directory, and build.sh uses it.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential git curl ca-certificates liburing-dev

V_COMMIT_FULL=93ad5d7e5d8f3f135c421af79fbc0b808051886a  # vlang/v master, 2026-10-30
# vlang/vc holds the generated bootstrap C (vc/v.c). Pinned to the newest vc commit that
# tracks a V commit which is an ancestor of V_COMMIT_FULL (its message says which one).
VC_COMMIT_FULL=a132e47985f9ae8f993469ace72eee9f270972a0  # tracks V 93ad5d7

rm -rf /opt/twelve-v
git init -q /opt/twelve-v
git -C /opt/twelve-v fetch -q --depth 1 https://github.com/vlang/v "$V_COMMIT_FULL"
git -C /opt/twelve-v checkout -q FETCH_HEAD
[ "$(git -C /opt/twelve-v rev-parse HEAD)" = "$V_COMMIT_FULL" ]

git init -q /opt/twelve-v/vc
git -C /opt/twelve-v/vc fetch -q --depth 1 https://github.com/vlang/vc "$VC_COMMIT_FULL"
git -C /opt/twelve-v/vc checkout -q FETCH_HEAD
[ "$(git -C /opt/twelve-v/vc rev-parse HEAD)" = "$VC_COMMIT_FULL" ]

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cd /opt/twelve-v
# v1 falls back to a cached 0.5.2 compiler when its own build fails, and that compiler is
# not the pinned commit. An empty cache makes a fallback impossible, and the log check below
# turns one into an error if it happens anyway.
export XDG_CACHE_HOME="$work/cache"
mkdir -p "$XDG_CACHE_HOME"
bootstrap_log="$work/bootstrap.log"
{ sh cmd/tools/vc_compat.sh vc/v.c cc -std=gnu11 -w -DCUSTOM_DEFINE_v1_fallback -o v1 -- -lm -lpthread &&
	./v1 -no-parallel -o v2 -gc none cmd/v; } 2>&1 | tee "$bootstrap_log"
if grep -q -e 'retrying with' -e 'v1-fallback' "$bootstrap_log"; then
	echo "v1 fell back to a cached compiler; the build is not the pinned commit" >&2
	exit 1
fi
rm -f v1
mv v2 v
chmod -R a+rX /opt/twelve-v
/opt/twelve-v/v version
echo "V source: vlang/v $V_COMMIT_FULL, vlang/vc $VC_COMMIT_FULL" | tee /opt/twelve-v/PINNED
