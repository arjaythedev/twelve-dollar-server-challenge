#!/usr/bin/env bash
# Builds the app as a normal user: installs packages into ./lib from a dated Posit Package Manager snapshot.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p lib
Rscript -e '
options(repos = c(CRAN = "https://packagemanager.posit.co/cran/__linux__/noble/2026-10-07"))
install.packages(c("plumber2", "yyjsonr", "DBI", "RSQLite", "jose", "rlang"), lib = "lib")
'
