#!/usr/bin/env bash
# Builds build/lib/*.jar (app + pinned deps) as a normal user.
set -euo pipefail
cd "$(dirname "$0")"
if [ -x /opt/jdk-25/bin/java ]; then export JAVA_HOME=/opt/jdk-25; fi
GRADLE=/opt/gradle-9.8.1/bin/gradle
[ -x "$GRADLE" ] || GRADLE="$(command -v gradle)"
"$GRADLE" --no-daemon --console=plain -q build
ls build/lib
