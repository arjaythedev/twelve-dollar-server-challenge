#!/usr/bin/env bash
set -euo pipefail
DEBIAN_FRONTEND=noninteractive apt-get update && apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip sqlite3
