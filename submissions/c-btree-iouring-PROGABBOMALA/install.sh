#!/usr/bin/env bash
# Once, as root, on a clean Ubuntu 24.04.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential ca-certificates curl unzip libssl-dev >/dev/null
