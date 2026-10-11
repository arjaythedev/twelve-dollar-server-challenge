#!/usr/bin/env bash
set -euo pipefail

apt-get update
apt-get install -y openjdk-21-jdk ca-certificates

update-alternatives --set java /usr/lib/jvm/java-21-openjdk-amd64/bin/java
update-alternatives --set javac /usr/lib/jvm/java-21-openjdk-amd64/bin/javac