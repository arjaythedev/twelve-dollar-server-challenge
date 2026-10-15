#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends python3 python3-dev python3-venv build-essential libsqlite3-dev libssl-dev ca-certificates
