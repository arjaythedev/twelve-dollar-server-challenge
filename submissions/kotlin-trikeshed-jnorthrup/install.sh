#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler (SQLite amalgamation), git (the TrikeShed subrepo),
# Oracle GraalVM for JDK 25.0.4 and Gradle 9.6.1, both from their official archives, checked against pinned SHA-256 sums.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl git unzip

fetch() { # fetch <url> <sha256> <file>
  curl -fsSL --retry 5 --retry-all-errors --retry-delay 3 "$1" -o "$3"
  echo "$2  $3" | sha256sum -c -
}

cd "$(mktemp -d)"
# Each toolchain lands under a temporary name and is renamed into place once complete.
if [ ! -x /opt/graalvm-jdk-25.0.4/bin/java ]; then
  fetch https://download.oracle.com/graalvm/25/archive/graalvm-jdk-25.0.4_linux-x64_bin.tar.gz \
    76007c309f821aaf435bce63162ea0395587fc77350801c81643fe7feea37276 graalvm.tar.gz
  rm -rf /opt/graalvm-jdk-25.0.4 /opt/.graalvm-jdk-25.0.4 && mkdir -p /opt/.graalvm-jdk-25.0.4
  tar -xzf graalvm.tar.gz -C /opt/.graalvm-jdk-25.0.4 --strip-components=1
  mv /opt/.graalvm-jdk-25.0.4 /opt/graalvm-jdk-25.0.4
  rm -f graalvm.tar.gz
fi
ln -sfn /opt/graalvm-jdk-25.0.4 /opt/graalvm

if [ ! -x /opt/gradle-9.6.1/bin/gradle ]; then
  fetch https://services.gradle.org/distributions/gradle-9.6.1-bin.zip \
    9c0f7faeeb306cb14e4279a3e084ca6b596894089a0638e68a07c945a32c9e14 gradle.zip
  rm -rf /opt/gradle-9.6.1 /opt/.gradle-unzip && unzip -q gradle.zip -d /opt/.gradle-unzip
  mv /opt/.gradle-unzip/gradle-9.6.1 /opt/gradle-9.6.1 && rmdir /opt/.gradle-unzip
  rm -f gradle.zip
fi
ln -sfn /opt/gradle-9.6.1 /opt/gradle

/opt/graalvm/bin/java -version
