#!/usr/bin/env bash
set -euo pipefail

apt-get update && apt-get install -y build-essential nasm libsqlite3-dev libssl-dev
