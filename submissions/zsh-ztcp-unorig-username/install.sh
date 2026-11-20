#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C toolchain to build zsh from source (build.sh),
# the SQLite library and headers for the zsh/sqlite module, and ncurses, which zsh's configure requires.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential libsqlite3-dev libncurses-dev curl ca-certificates xz-utils
