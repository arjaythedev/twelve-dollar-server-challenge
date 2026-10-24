#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs R 4.6.1 from Posit's r-builds.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
R_VERSION=4.6.1
apt-get update
apt-get install -y --no-install-recommends curl ca-certificates
curl -fsSLo /tmp/r.deb "https://cdn.posit.co/r/ubuntu-2404/pkgs/r-${R_VERSION}_1_$(dpkg --print-architecture).deb"
apt-get install -y --no-install-recommends /tmp/r.deb
rm -f /tmp/r.deb
ln -sf "/opt/R/${R_VERSION}/bin/R" /usr/local/bin/R
ln -sf "/opt/R/${R_VERSION}/bin/Rscript" /usr/local/bin/Rscript
Rscript --version
