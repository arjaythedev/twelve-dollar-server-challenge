#!/usr/bin/env bash
# Prepare a private patched tree; the downloaded release remains the control.
set -euo pipefail
source_dir=${1:?upstream source directory}
destination=${2:?generated source directory}
patch_file=${3:?PCB lookup patch}
parent=$(dirname "$destination")
mkdir -p "$parent"
stage=$(mktemp -d "$parent/lwip-index.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
cp -R "$source_dir/src" "$stage/src"
cp "$source_dir/COPYING" "$stage/COPYING"
patch --batch --fuzz=0 --strip=1 --directory="$stage" < "$patch_file"
sha256sum "$patch_file" | cut -d' ' -f1 > "$stage/.index-ready"
if ! mv -T "$stage" "$destination" 2>/dev/null; then
    cmp "$stage/.index-ready" "$destination/.index-ready"
    touch "$destination/.index-ready"
fi
