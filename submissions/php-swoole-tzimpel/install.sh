#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: PHP 8.3 (Ubuntu's own packages, linked to its SQLite 3.45.1)
# plus what build.sh needs to compile the Swoole extension from source.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  php8.3-cli php8.3-dev php8.3-sqlite3 php8.3-opcache build-essential autoconf curl ca-certificates
php --version
