#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends gcc libc6-dev binutils libsqlite3-dev libssl-dev ca-certificates curl tar