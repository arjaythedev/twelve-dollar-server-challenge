#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Swift 6.4.2 (official tarball, sha256-checked) -> /opt/swift. The
# tarball is unpacked as it downloads and never stored, and what a release build doesn't use (the debugger, the
# editor and documentation tools) is left out, so the install needs less than half the disk.
set -euo pipefail
SWIFT_VERSION=6.4.2
SHA256=2e9ebec040ad459dd89a6d093a02d8314939135104850fae4d5c0f174fb3ba8c

apt-get update -q
apt-get install -y -q \
  binutils curl git gnupg2 libc6-dev libcurl4-openssl-dev libedit2 libgcc-13-dev \
  libncurses-dev libpython3-dev libsqlite3-0 libstdc++-13-dev libxml2-dev libz3-dev \
  pkg-config tzdata unzip zlib1g-dev ca-certificates

TARBALL="swift-$SWIFT_VERSION-RELEASE-ubuntu24.04.tar.gz"
URL="https://download.swift.org/swift-$SWIFT_VERSION-release/ubuntu2404/swift-$SWIFT_VERSION-RELEASE/$TARBALL"
rm -rf /opt/swift && mkdir -p /opt/swift
# sha256sum reads a copy of the download through a FIFO while tar unpacks it; if the sum doesn't match, everything
# unpacked is removed and the install fails.
FIFO=$(mktemp -u) && mkfifo "$FIFO"
sha256sum < "$FIFO" > "$FIFO.sum" &
curl -fsSL "$URL" | tee -p "$FIFO" | tar -C /opt/swift -xz --strip-components=1 \
  --exclude='usr/bin/lldb*' --exclude='usr/lib/liblldb*' --exclude='usr/lib/python3*' \
  --exclude='usr/bin/sourcekit-lsp' --exclude='usr/lib/libsourcekitdInProc.so' --exclude='usr/bin/clangd' \
  --exclude='usr/bin/docc' --exclude='usr/share/docc' --exclude='usr/share/doc' --exclude='usr/share/man' \
  --exclude='usr/bin/swift-format'
wait
if [ "$(cut -d' ' -f1 "$FIFO.sum")" != "$SHA256" ]; then
  rm -rf /opt/swift "$FIFO" "$FIFO.sum"
  echo "swift tarball: sha256 mismatch" >&2
  exit 1
fi
rm -f "$FIFO" "$FIFO.sum"
for bin in swift swiftc; do ln -sf "/opt/swift/usr/bin/$bin" "/usr/local/bin/$bin"; done
swift --version
