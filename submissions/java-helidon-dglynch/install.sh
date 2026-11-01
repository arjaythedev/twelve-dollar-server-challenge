#!/usr/bin/env bash
set -euo pipefail

apt-get update -qq && apt-get install -y curl unzip java-common

curl -fsSLO https://download.oracle.com/java/27/archive/jdk-27.0.1.0.2_linux-x64_bin.deb
apt-get install -y ./jdk-27.0.1.0.2_linux-x64_bin.deb
update-java-alternatives --set jdk-27.0.1.0.2-oracle-x64

curl -fsSLO https://services.gradle.org/distributions/gradle-9.8.2-bin.zip
unzip -oq gradle-9.8.2-bin.zip -d /opt/gradle
ln -sfn /opt/gradle/gradle-9.8.2/bin/gradle /usr/local/bin/gradle
