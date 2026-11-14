#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/usr/local/go/bin:/usr/local/bin:${PATH}"
if [[ -z "${TRUX_LIBJWT_PREFIX:-}" && ! -d /opt/libjwt && -d "${HOME}/.cache/trux/libjwt" ]]; then
  export TRUX_LIBJWT_PREFIX="${HOME}/.cache/trux/libjwt"
fi
export TRUX_LIBJWT_PREFIX="${TRUX_LIBJWT_PREFIX:-/opt/libjwt}"
if ! command -v trux >/dev/null 2>&1; then
  echo "trux is not on PATH; run install.sh as root first" >&2
  exit 1
fi
mkdir -p bin
trux build -o bin/server server.tx
