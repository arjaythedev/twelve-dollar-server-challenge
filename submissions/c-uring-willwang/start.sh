#!/usr/bin/env bash
set -euo pipefail
case "${1:-}" in
  '') exec "$(dirname "$0")/bin/server" ;;
  --bypass)
    shift
    binary=server-bypass-fixed
    for argument in "$@"; do
      case "$argument" in
        --packet-t*|--h|--he|--hel|--help) binary=server-bypass; break ;;
        --) break ;;
      esac
    done
    exec "$(dirname "$0")/bin/$binary" "$@"
    ;;
  *) echo 'Usage: start.sh [--bypass --help | --bypass OPTIONS]' >&2; exit 2 ;;
esac
