#!/usr/bin/env bash
# Run the repository's unmodified harness with an existing seed, keeping scratch
# files inside this submission. Usage: SEED_DIR=/path/to/seed bash validate.sh
set -euo pipefail
cd "$(dirname "$0")"
APP=$PWD
ROOT=$(cd ../.. && pwd)
mkdir -p build/validation
cp -R "$ROOT/test" build/validation/
ln -sfn "${SEED_DIR:?set SEED_DIR to an existing challenge seed directory}" build/validation/seed
PORT=${PORT:-18372} bash build/validation/test/run.sh "$APP"
