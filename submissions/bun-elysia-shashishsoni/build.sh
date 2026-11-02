#!/usr/bin/env bash
# Builds the app as a normal user. Pinned deps via bun.lock.
set -euo pipefail
cd "$(dirname "$0")"
bun install --frozen-lockfile --production
