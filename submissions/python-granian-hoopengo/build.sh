#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

# Create venv strictly on final CPython 3.15.0.
#
# IMPORTANT: `uv venv --python 3.15` (without patch version) makes uv take the
# first matching build, which is managed 3.15.0a5 (alpha). In alpha there is no
# `PyType_FromSlots`, and granian 2.8.4 (built with PyO3 against the final 3.15
# ABI) fails on import:
#   ImportError: dlopen(...): symbol not found in flat namespace '_PyType_FromSlots'
# Therefore: exact version 3.15.0 + preference for the system interpreter
# (python3.15 from deadsnakes, installed by install.sh).
uv venv --clear --python 3.15.0 --python-preference system

# Activate the created environment
source .venv/bin/activate

# Install dependencies with uv (much faster than pip)
# Exact versions — as in uv.lock (rule 10: pin dependencies).
uv pip install \
    "granian[uvloop]==2.8.4" \
    "apsw==3.54.0.0" \
    "orjson==3.13.0"

# Fix exact versions for reproducibility
uv pip freeze > requirements.txt

# Check that the interpreter is really final 3.15.0 (not 3.15.0aX/bX).
# Without this check the ABI error surfaces only at runtime, after a "successful" build.
python - <<'PY'
import sys
assert sys.version_info[:3] == (3, 15, 0), (
    f"need final CPython 3.15.0, not {sys.version.split()[0]}: "
    "alpha/beta lack PyType_FromSlots, and granian cannot import"
)
import apsw  # noqa: F401
import granian
import orjson  # noqa: F401
import uvloop  # noqa: F401
print(f"OK: CPython {sys.version.split()[0]}, granian {granian.__version__}")
PY