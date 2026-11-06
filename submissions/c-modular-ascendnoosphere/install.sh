#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends build-essential curl ca-certificates unzip tar pkg-config \
    libssl-dev=3.0.13-0ubuntu3.16 libssl3t64=3.0.13-0ubuntu3.16 openssl=3.0.13-0ubuntu3.16
