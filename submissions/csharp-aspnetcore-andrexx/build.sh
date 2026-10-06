#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
dotnet restore FeedApi.csproj --locked-mode
dotnet publish FeedApi.csproj -c Release --no-restore -o bin/publish --self-contained false
