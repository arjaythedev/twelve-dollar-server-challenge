#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: the host toolchain for the SH-4
# cross compiler, Flycast's build deps, and Xvfb + Mesa so Flycast has a screen.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential git curl wget ca-certificates bzip2 xz-utils patch \
  texinfo flex bison python3 libelf-dev libpng-dev libjpeg-dev \
  cmake ninja-build pkg-config \
  libsdl2-dev libgl-dev libegl-dev libgl1-mesa-dri libglx-mesa0 libzip-dev zlib1g-dev libcurl4-openssl-dev \
  xvfb unzip
