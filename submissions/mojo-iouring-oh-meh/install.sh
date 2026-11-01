#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: C toolchain for the shim, and the
# pinned Mojo 1.2.0 toolchain (a prebuilt 40 MB wheel, like a Go or Rust tarball)
# into /opt/mojo so build.sh can run it as a normal user.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential curl unzip ca-certificates python3 python3-venv >/dev/null

MOJO_VERSION=1.2.0
python3 -m venv /opt/mojo
/opt/mojo/bin/pip install -q --upgrade pip
/opt/mojo/bin/pip install -q "mojo==$MOJO_VERSION"
chmod -R a+rX /opt/mojo
/opt/mojo/bin/mojo --version
