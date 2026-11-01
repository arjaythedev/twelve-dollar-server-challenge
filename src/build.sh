#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B .build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build --parallel 2
