#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
export SHIM_CURSORS=1
exec ./server