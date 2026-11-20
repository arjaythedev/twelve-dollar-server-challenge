#!/usr/bin/env bash
# Runs the server in the foreground.
set -euo pipefail
cd "$(dirname "$0")"
# Fixed settings of this build (see README, Design); nothing else comes in but the SPEC.md env vars.
exec bin/server --answers=64 --wwin=10 --defacc=1
