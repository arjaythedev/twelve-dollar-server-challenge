#!/usr/bin/env bash
# Nothing to compile or download: PyPy and its bundled cffi are the only dependencies.
# Byte-compiling up front just checks the source and saves a few milliseconds at start.
set -euo pipefail
cd "$(dirname "$0")"
pypy3 -m compileall -q server.py
