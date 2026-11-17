#!/usr/bin/env bash
# Installs compiler and dependencies for the Brainfuck server on clean Ubuntu 24.04.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q build-essential libsqlite3-dev libssl-dev curl
