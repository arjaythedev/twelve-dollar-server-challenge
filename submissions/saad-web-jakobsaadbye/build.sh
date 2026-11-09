#!/usr/bin/env bash
# Builds the server as a normal user: the Saad compiler from source at a pinned commit (with LLVM), then the program
# with it, optimized by LLVM, linked with Ubuntu's SQLite. SAAD_SOURCE builds with a Saad repository that is there
# instead, e.g a checkout of your own
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SAAD_COMMIT="${SAAD_COMMIT:-764e8b9e5dc5eb03afbfb23270f52bb90cc985a6}"
SAAD="${SAAD_SOURCE:-$HERE/build/saad}"

if [ -z "${SAAD_SOURCE:-}" ]; then
    if [ ! -d "$SAAD/.git" ]; then
        rm -rf "$SAAD"
        git clone --quiet https://github.com/jakobsaadbye/Saad "$SAAD"
    fi
    git -C "$SAAD" fetch --quiet origin "$SAAD_COMMIT"
    git -C "$SAAD" checkout --quiet FETCH_HEAD
fi

bash "$SAAD/build.sh"
"$SAAD/bin/saad" build "$HERE/main.sd" --elf --llvm
mkdir -p "$HERE/bin"
bash "$SAAD/tools/linux/link.sh" "$HERE/build/out.o" "$HERE/bin/server"
echo "Built $HERE/bin/server"
