#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. The build downloads its own pinned liburing and SQLite sources.
# sqlite3, curl and openssl are used by the profile-guided build's training step (train/train.sh).
set -euo pipefail
apt-get update
apt-get install -y build-essential curl ca-certificates sqlite3 openssl
