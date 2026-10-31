#!/usr/bin/env bash
# install.sh: runs once as root on a clean Ubuntu 24.04
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
# ngspice 42 (the simulator, with XSPICE and d_cosim), Verilator 5.020 + g++ (to build the chip model
# d_cosim loads), socat (one process per connection), sqlite3 (the database CLI)
apt-get install -y ngspice=42+ds-3build1 verilator=5.020-1 g++ make socat sqlite3
