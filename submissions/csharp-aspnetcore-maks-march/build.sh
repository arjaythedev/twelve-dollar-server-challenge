#!/usr/bin/env bash
# Builds the app as a normal user: framework-dependent publish using the exact NuGet
# versions pinned in packages.lock.json (transitive dependencies included).
set -euo pipefail
cd "$(dirname "$0")"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 DOTNET_NOLOGO=1
dotnet publish -c Release -p:RestoreLockedMode=true -o bin
