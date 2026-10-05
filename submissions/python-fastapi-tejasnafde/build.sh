#!/usr/bin/env bash
# Builds the app as a normal user: a virtualenv with the pinned dependencies from requirements.txt.
set -euo pipefail
cd "$(dirname "$0")"
python3 -m venv --clear .venv
.venv/bin/pip install --disable-pip-version-check --no-input --quiet --only-binary=:all: -r requirements.txt
