#!/usr/bin/env bash
# build.sh: Ecko compiles at startup, so the build is a static check that refuses
# to continue on any error-severity finding.
set -euo pipefail
cd "$(dirname "$0")"
"${ECKO:-ecko}" check server.ecko
