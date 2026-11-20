#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the C toolchain for the shim and
# GNU Fortran 13 from Ubuntu's own packages. Nothing else.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential gfortran curl unzip ca-certificates >/dev/null
gfortran --version | head -1
