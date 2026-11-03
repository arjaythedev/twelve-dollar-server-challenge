#!/usr/bin/env bash
# Builds an AOT-compiled native executable (dependencies pinned in pubspec.lock).
set -euo pipefail
cd "$(dirname "$0")"
DART=/usr/local/dart-sdk/bin/dart
[ -x "$DART" ] || DART=dart
"$DART" pub get --enforce-lockfile
mkdir -p build
rm -f build/server
"$DART" compile exe bin/server.dart -o build/server
