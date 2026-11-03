#!/usr/bin/env bash
# Runs the server in the foreground. Configured by SQLITE_PATH, JWT_SECRET, HOST and PORT.
cd "$(dirname "$0")"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
exec /usr/local/lib/dotnet-sdk-10.0.401/dotnet bin/feed.dll
