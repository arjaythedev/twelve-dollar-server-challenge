#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
build_testing=OFF
build_target=server
if [[ "${1:-}" == "--tests" ]]; then
    build_testing=ON
    build_target=all
    shift
fi
cmake -S . -B .build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING="${build_testing}" "$@"
cmake --build .build --target "${build_target}" --parallel 2
