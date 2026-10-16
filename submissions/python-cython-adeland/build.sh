#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -d .venv ]; then python3 -m venv .venv; fi
.venv/bin/python -m pip install --disable-pip-version-check --quiet Cython==3.1.6 setuptools==80.9.0
.venv/bin/python tools/fetch_deps.py
.venv/bin/python setup.py build_ext --inplace --force
