#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential liburing-dev curl unzip ca-certificates
