#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: GNU Emacs 29.3 (terminal build) from the Ubuntu archive.
# It ships its own SQLite and JSON bindings, plus libgccjit for native compilation.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends emacs-nox=1:29.3+1-1ubuntu2
