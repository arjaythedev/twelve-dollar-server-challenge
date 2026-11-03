#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential curl unzip ca-certificates
