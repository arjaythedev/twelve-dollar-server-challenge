#!/usr/bin/env bash
# Builds the server as a normal user. Dependencies are pinned in Package.resolved.
set -euo pipefail
cd "$(dirname "$0")"

# SWIFT_BIN env override is handy for local (non-Ubuntu) testing.
if [ -n "${SWIFT_BIN:-}" ]; then
    SWIFT="$SWIFT_BIN"
elif [ -x /opt/swift/usr/bin/swift ]; then
    SWIFT=/opt/swift/usr/bin/swift
else
    SWIFT="$(command -v swift || true)"
fi
if [ -z "$SWIFT" ] || [ ! -x "$SWIFT" ]; then
    echo "Swift toolchain not found (set SWIFT_BIN or install Swift on PATH)" >&2
    exit 1
fi

# Release build; try cross-module optimization first, fall back if the flag is unsupported.
if ! "$SWIFT" build -c release -Xswiftc -cross-module-optimization --product server; then
    echo "cross-module-optimization failed, building without it"
    "$SWIFT" build -c release --product server
fi

mkdir -p bin
cp -f .build/release/server bin/server
