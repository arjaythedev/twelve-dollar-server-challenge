#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
[[ $(uname -s) == Linux ]] || { echo 'install.sh requires Ubuntu 24.04; it does not install packages on macOS.' >&2; exit 1; }
source /etc/os-release
[[ ${ID:-} == ubuntu && ${VERSION_ID:-} == 24.04 ]] || { echo 'Ubuntu 24.04 required' >&2; exit 1; }
[[ $EUID == 0 ]] || { echo 'Run install.sh as root' >&2; exit 1; }
source ./versions.env
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  "gnat-13=$GNAT_VERSION" "gcc-13=$GNAT_VERSION" "libgnat-13=$GNAT_VERSION" \
  "libsqlite3-dev=$SQLITE_VERSION" "libsqlite3-0=$SQLITE_VERSION" \
  "libssl-dev=$OPENSSL_VERSION" "libssl3t64=$OPENSSL_VERSION" \
  libc6-dev make
