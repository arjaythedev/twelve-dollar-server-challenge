#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ ! -x .work/build/server ]]; then echo 'Run build.sh first.' >&2; exit 1; fi
exec .work/build/server
