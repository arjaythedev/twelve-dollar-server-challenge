#!/usr/bin/env bash
# install.sh: Runs once as root on a clean Ubuntu 24.04 box.
set -euo pipefail

apt-get update -qq
apt-get install -y -qq openjdk-21-jdk-headless maven
