#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: GNU Octave 8.4 (the web server), its headers and
# mkoctfile (to compile the small C++ helper), and the SQLite headers and library it links against.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends octave=8.4.0-1build5 octave-dev=8.4.0-1build5 libsqlite3-dev
octave-cli --version | head -1
