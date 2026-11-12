#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
work=${1:?usage: build-client.sh WORK_DIRECTORY}
source_dir="$repo/submissions/c-iouring-moneebullah25"
mkdir -p "$work"
patch --batch --quiet -o "$work/mload.c" "$source_dir/tools/mload.c" "$here/mload.patch"
cc -O3 -march=native -I"$source_dir/src" "$work/mload.c" -o "$work/mload"
