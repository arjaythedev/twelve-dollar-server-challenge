#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler, make, curl and git.
# That is all root does. The Nexium compiler is built by build.sh, as the
# normal build user, from its source at a pinned commit.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates git
