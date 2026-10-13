#!/usr/bin/env bash
# Required by the challenge. Runs once as root on a clean Ubuntu 24.04, which may not have python3 yet.
set -e; apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends python3 python3-venv
