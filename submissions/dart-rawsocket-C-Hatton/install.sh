#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the Dart SDK (official zip, pinned) and libsqlite3.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
DART_VERSION=3.13.2
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl unzip libsqlite3-0 libsqlite3-dev
rm -rf /usr/local/dart-sdk
curl -fsSL "https://storage.googleapis.com/dart-archive/channels/stable/release/$DART_VERSION/sdk/dartsdk-linux-x64-release.zip" -o /tmp/dartsdk.zip
unzip -q /tmp/dartsdk.zip -d /usr/local
rm /tmp/dartsdk.zip
chmod -R a+rX /usr/local/dart-sdk
/usr/local/dart-sdk/bin/dart --version
