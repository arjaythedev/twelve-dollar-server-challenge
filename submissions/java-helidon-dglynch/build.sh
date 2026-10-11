#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")" && gradle build installDist -x test --no-daemon
