#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs PyPy (Python 3.9 dialect) from Ubuntu's universe.
# That is the whole toolchain: the server uses only PyPy's standard library and its bundled cffi.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends pypy3=7.3.15+dfsg-1build3
pypy3 --version
