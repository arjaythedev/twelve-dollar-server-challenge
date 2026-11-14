#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs build-essential, curl, unzip
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential curl ca-certificates unzip
