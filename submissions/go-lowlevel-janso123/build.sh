#!/usr/bin/env bash
# Builds the server as a normal user (downloads the pinned Go modules from go.sum). See Makefile.
set -euo pipefail
cd "$(dirname "$0")"
make
