#!/usr/bin/env bash
set -euxo pipefail
cd "$(dirname "$0")"
#export HOST=0.0.0.0
#export PORT=80
exec ./bin/server
