#!/usr/bin/env bash
# install.sh: runs once as root on a clean Ubuntu 24.04
set -euo pipefail
apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y bash-builtins sqlite3
