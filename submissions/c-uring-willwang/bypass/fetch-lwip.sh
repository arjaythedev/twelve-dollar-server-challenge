#!/usr/bin/env bash
# Fetch the upstream BSD-licensed lwIP source. Its COPYING stays beside src/.
set -euo pipefail
destination=${1:?lwIP destination directory}
tag=STABLE-2_2_1_RELEASE
digest=ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539
parent=$(dirname "$destination")
mkdir -p "$parent"
stage=$(mktemp -d "$parent/lwip-fetch.XXXXXXXX")
trap 'rm -rf -- "$stage"' EXIT
curl -fsSL --retry 3 "https://codeload.github.com/lwip-tcpip/lwip/tar.gz/refs/tags/$tag" -o "$stage/upstream.tar.gz"
echo "$digest  $stage/upstream.tar.gz" | sha256sum --check --status
tar -xzf "$stage/upstream.tar.gz" -C "$stage" "lwip-$tag/src" "lwip-$tag/COPYING"
if ! mv -T "$stage/lwip-$tag" "$destination" 2>/dev/null; then
    # Another builder may have completed the same atomic directory rename.
    cmp "$stage/lwip-$tag/src/include/lwip/init.h" "$destination/src/include/lwip/init.h"
    cmp "$stage/lwip-$tag/COPYING" "$destination/COPYING"
fi
