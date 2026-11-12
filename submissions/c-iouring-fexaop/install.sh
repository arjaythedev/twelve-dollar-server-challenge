#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler and the tools build.sh needs.
set -euo pipefail
apt-get update
apt-get install -y build-essential curl ca-certificates unzip
