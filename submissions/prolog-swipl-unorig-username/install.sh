#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: SWI-Prolog 9.0.4 (the noble package, pinned), a C compiler
# for the small SQLite binding that build.sh compiles with swipl-ld, and the SQLite headers
# (libsqlite3-dev 3.45.1, the noble build; left unpinned so a security update cannot break the install).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
V=9.0.4+dfsg-3.1ubuntu4
apt-get update
apt-get install -y --no-install-recommends swi-prolog-nox=$V swi-prolog-core=$V swi-prolog-core-packages=$V \
  gcc libc6-dev libsqlite3-dev
swipl --version
