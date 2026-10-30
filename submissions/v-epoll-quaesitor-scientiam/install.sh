#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the C toolchain and builds the pinned
# V compiler into /opt/twelve-v/v. The source is two archives of the pinned commits, made with
# `git archive`: V master b5bcdbb and the vc bootstrap 7ba7d3e. Their SHA-256 is checked before
# anything is unpacked, so the build does not depend on git history or on the state of GitHub
# repos. The bootstrap follows the Makefile: vc/v.c builds v1 through the compat script, v1
# builds v2 from cmd/v, and v2 is installed. It stops there: the Makefile's last step rebuilds
# the compiler with itself, which ran a 2 GB box out of memory. v2 finds vlib from any
# directory, and build.sh uses it.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential git curl ca-certificates unzip liburing-dev

# Release assets of the submission's fork, tag v-src-93ad5d7. The archives hold vlang/v at
# 93ad5d7e5d8f3f135c421af79fbc0b808051886a (master, 2026-10-30) and vlang/vc at
# a132e47985f9ae8f993469ace72eee9f270972a0 (tracks V 93ad5d7).
ARCHIVE_BASE=https://github.com/quaesitor-scientiam/twelve-dollar-server-challenge/releases/download/v-src-93ad5d7
V_ARCHIVE=v-93ad5d7.zip
V_ARCHIVE_SHA256=abfb3a128d626b595d035c5a632feac3bff6008cc278305462b6c500db9dd393
VC_ARCHIVE=vc-a132e47.zip
VC_ARCHIVE_SHA256=971a692b362467586d61124be07301696eb89d0590e7bdface63b9628f1cae45

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# fetch_verified URL SHA256 DEST downloads URL to DEST and fails unless the hash matches.
fetch_verified() {
	curl -fsSL --retry 3 -o "$3" "$1"
	echo "$2  $3" | sha256sum -c -
}
fetch_verified "$ARCHIVE_BASE/$V_ARCHIVE" "$V_ARCHIVE_SHA256" "$work/$V_ARCHIVE"
fetch_verified "$ARCHIVE_BASE/$VC_ARCHIVE" "$VC_ARCHIVE_SHA256" "$work/$VC_ARCHIVE"

# Both archives have a top-level directory (v/ and vc/), so they unpack into /opt/twelve-v.
rm -rf /opt/twelve-v
unzip -q "$work/$V_ARCHIVE" -d "$work"
mv "$work/v" /opt/twelve-v
unzip -q "$work/$VC_ARCHIVE" -d /opt/twelve-v

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
