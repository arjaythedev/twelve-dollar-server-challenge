#!/usr/bin/env bash
# Compiles the one C++ helper (ox.cc -> ox.oct) against Octave and the system SQLite.
set -euo pipefail
cd "$(dirname "$0")"
mkoctfile -O2 -Wall -o ox.oct ox.cc -lsqlite3
