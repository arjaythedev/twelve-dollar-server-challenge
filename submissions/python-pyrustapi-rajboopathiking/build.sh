#!/usr/bin/env bash
# Builds the app as a normal user: a virtualenv with pinned dependencies from requirements.txt.
set -euo pipefail
cd "$(dirname "$0")"

[ -f "$HOME/.cargo/env" ] && source "$HOME/.cargo/env" || true
export PATH="$HOME/.cargo/bin:$PATH"

python3 -m venv --clear .venv
.venv/bin/pip install --disable-pip-version-check --no-input --quiet -r requirements.txt
