#!/usr/bin/env bash
# Required by the challenge; the logic is in infra/ops.py.
exec "${PYTHON:-python3}" "$(dirname "$0")/infra/ops.py" build
