#!/usr/bin/env bash
# Byte-compiles the server as a normal user.
set -euo pipefail
cd "$(dirname "$0")"
emacs --batch -Q --eval '(setq byte-compile-error-on-warn t)' -f batch-byte-compile server.el
