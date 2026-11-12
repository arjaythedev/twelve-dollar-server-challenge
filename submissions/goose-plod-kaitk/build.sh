#!/usr/bin/env bash
# Builds bin/server as a normal user. goose build fetches Plod, goose-http,
# goose-hash and goose-sqlite at the tags server.goose declares, writes the
# program as C, and compiles it with SQLite's amalgamation, which goose-sqlite
# carries.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
goose build -o bin/server server.goose
