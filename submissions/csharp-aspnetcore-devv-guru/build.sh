#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
SDK_ROOT=/opt/twelve-dollar-dotnet/10.0.401
if [ -x "$SDK_ROOT/dotnet" ]; then
  DOTNET="$SDK_ROOT/dotnet"
else
  DOTNET="$(command -v dotnet)"
fi
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
"$DOTNET" restore FeedApi.csproj --locked-mode
"$DOTNET" publish FeedApi.csproj -c Release -r linux-x64 --self-contained true --no-restore -o bin/publish
