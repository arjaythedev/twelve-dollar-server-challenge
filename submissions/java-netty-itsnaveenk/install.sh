#!/usr/bin/env bash
# install.sh: Runs once as root on a clean Ubuntu 24.04 box.
set -euo pipefail

apt-get update -qq
apt-get install -y -qq openjdk-21-jdk-headless maven

for jdir in /usr/lib/jvm/java-21-openjdk-amd64 /usr/lib/jvm/java-21-openjdk-arm64; do
  if [ -x "$jdir/bin/java" ]; then
    update-alternatives --set java "$jdir/bin/java" 2>/dev/null || true
    update-alternatives --set javac "$jdir/bin/javac" 2>/dev/null || true
    break
  fi
done
