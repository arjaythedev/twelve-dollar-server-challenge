#!/usr/bin/env bash
# Builds the app as a normal user: restores the pinned NuGet packages (packages.lock.json) and publishes a Release build.
set -euo pipefail
cd "$(dirname "$0")"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
dotnet publish Feed.csproj -c Release -o bin/publish -p:RestoreLockedMode=true
