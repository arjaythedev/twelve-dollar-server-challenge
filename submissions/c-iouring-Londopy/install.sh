#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler, make and curl.
# No liburing: the server talks to io_uring through raw syscalls.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates
