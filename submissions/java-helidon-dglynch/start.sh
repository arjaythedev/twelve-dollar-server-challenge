#!/usr/bin/env bash
set -euo pipefail

JAVA_HOME="$(dirname "$(dirname "$(readlink -f /etc/alternatives/java)")")"
export JAVA_HOME
exec "$(dirname "$0")/app/build/install/app/bin/app"
