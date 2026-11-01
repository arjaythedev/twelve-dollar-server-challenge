#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. The build downloads its own pinned liburing and SQLite sources.
set -euo pipefail
apt-get update
apt-get install -y build-essential curl ca-certificates
