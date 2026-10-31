#!/usr/bin/env bash
# Builds everything from source into build/ (each step is skipped once its stamp matches):
#   1. SQLite 3.54.1 amalgamation (pinned, sha256-checked)
#   2. KallistiOS 2.2.2 (pinned tarball, sha256-checked) + patches/kos/*.patch
#   3. the sh-elf cross toolchain: GCC 13.2.0, binutils 2.43, newlib 4.3 via KallistiOS's dc-chain
#      (the slow part: well over an hour on one vCPU)
#   4. KallistiOS itself
#   5. Flycast at a pinned commit (git, with the submodules it needs) + patches/flycast/*.patch
#   6. the Dreamcast program in guest/ -> build/dreamcast-sql.elf
set -euo pipefail
D="$(cd "$(dirname "$0")" && pwd)"
B="$D/build"
mkdir -p "$B"
J="$(nproc 2> /dev/null || sysctl -n hw.ncpu)"
log() { echo "== $*"; }
sha256() { if sha256sum --version > /dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
fetch() { # fetch <url> <sha256> <file>
  if ! { [ -f "$3" ] && [ "$(sha256 "$3" | cut -d' ' -f1)" = "$2" ]; }; then
    curl -fsSL --retry 3 -o "$3" "$1"
    [ "$(sha256 "$3" | cut -d' ' -f1)" = "$2" ] || { echo "$3: sha256 mismatch"; exit 1; }
  fi
}
retry() { for i in 1 2 3 4; do "$@" && return 0; echo "retrying: $*"; sleep $((i * 5)); done; return 1; }
stamp_ok() { [ "$(cat "$1" 2> /dev/null)" = "$2" ]; }

SQLITE_URL=https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
SQLITE_SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
KOS_URL=https://github.com/KallistiOS/KallistiOS/archive/refs/tags/v2.2.2.tar.gz
KOS_SHA256=8d363f759bf3075c6cbbb5b99499f898a9396bbebe7c40af18b33124637104bd
FLYCAST_URL=https://github.com/flyinghead/flycast.git
FLYCAST_REV=1b63c8a3677cc8eb1292863c4fe8fc8f94674fc8
# the submodules the Linux build needs (their commits are pinned by FLYCAST_REV)
FLYCAST_SUBMODULES="core/deps/libchdr core/deps/asio core/deps/xbyak core/deps/libjuice core/deps/websocketpp
  core/deps/tinygettext core/deps/freetype core/deps/rcheevos core/deps/DreamPicoPort-API"

# 1. SQLite
if ! stamp_ok "$B/sqlite/STAMP" "$SQLITE_SHA256"; then
  log "SQLite"
  fetch "$SQLITE_URL" "$SQLITE_SHA256" "$B/sqlite.zip"
  rm -rf "$B/sqlite" "$B/sqlite-amalgamation-3540100"
  (cd "$B" && unzip -q sqlite.zip && mv sqlite-amalgamation-3540100 sqlite)
  echo "$SQLITE_SHA256" > "$B/sqlite/STAMP"
fi

# 2. KallistiOS sources
kos_stamp="$KOS_SHA256 $(cat "$D"/patches/kos/*.patch | sha256 | cut -c1-16)"
if ! stamp_ok "$B/kos/STAMP" "$kos_stamp"; then
  log "KallistiOS sources"
  fetch "$KOS_URL" "$KOS_SHA256" "$B/kos.tar.gz"
  rm -rf "$B/kos" "$B/KallistiOS-2.2.2"
  (cd "$B" && tar xzf kos.tar.gz && mv KallistiOS-2.2.2 kos)
  for p in "$D"/patches/kos/*.patch; do patch -d "$B/kos" -p1 < "$p"; done
  # Nothing here plays sound, so the AICA (ARM7) sound driver is an empty stub rather than
  # either KallistiOS's prebuilt binary or a second cross compiler.
  : > "$B/kos/kernel/arch/dreamcast/sound/arm/stream.drv.prebuilt"
  sed -e "s#^export KOS_BASE=.*#export KOS_BASE=\"$B/kos\"#" \
      -e "s#^export KOS_CC_BASE=.*#export KOS_CC_BASE=\"$B/toolchain/sh-elf\"#" \
      -e "s#^export DC_ARM_BASE=.*#export DC_ARM_BASE=\"$B/toolchain/arm-eabi\"#" \
      -e "s#^export DC_TOOLS_BASE=.*#export DC_TOOLS_BASE=\"$B/toolchain/bin\"#" \
      "$B/kos/doc/environ.sh.sample" > "$B/kos/environ.sh"
  rm -f "$B/toolchain/STAMP" "$B/kos/lib/dreamcast/libkallisti.a"
  echo "$kos_stamp" > "$B/kos/STAMP"
fi

# 3. sh-elf toolchain (dc-chain's "stable" profile pins the GCC/binutils/newlib versions)
if ! stamp_ok "$B/toolchain/STAMP" "$KOS_SHA256"; then
  log "sh-elf toolchain (GCC 13.2.0; slow)"
  cd "$B/kos/utils/dc-chain"
  sed -e "s#^sh_toolchain_path=.*#sh_toolchain_path=$B/toolchain/sh-elf#" \
      -e "s#^arm_toolchain_path=.*#arm_toolchain_path=$B/toolchain/arm-eabi#" \
      -e "s#^makejobs=.*#makejobs=$J#" -e "s#^enable_cpp=.*#enable_cpp=0#" \
      Makefile.default.cfg > Makefile.cfg
  make build > "$B/toolchain.log" 2>&1 || { tail -40 "$B/toolchain.log"; exit 1; }
  make clean > /dev/null 2>&1 || true
  echo "$KOS_SHA256" > "$B/toolchain/STAMP"
fi

# 4. KallistiOS
if [ ! -f "$B/kos/lib/dreamcast/libkallisti.a" ]; then
  log "KallistiOS"
  (cd "$B/kos" && set +u && . ./environ.sh && make -j"$J" > "$B/kos.log" 2>&1) || { tail -40 "$B/kos.log"; exit 1; }
fi

# 5. Flycast (headless needs nothing special: start.sh gives it an Xvfb screen)
fly_stamp="$FLYCAST_REV $(cat "$D"/patches/flycast/*.patch | sha256 | cut -c1-16)"
if ! stamp_ok "$B/flycast-src/STAMP" "$fly_stamp"; then
  log "Flycast sources"
  rm -rf "$B/flycast-src" "$B/flycast"
  git init -q "$B/flycast-src"
  retry git -C "$B/flycast-src" fetch -q --depth 1 "$FLYCAST_URL" "$FLYCAST_REV"
  git -C "$B/flycast-src" checkout -q FETCH_HEAD
  # shellcheck disable=SC2086
  retry git -C "$B/flycast-src" submodule -q update --init --recursive --depth 1 $FLYCAST_SUBMODULES
  for p in "$D"/patches/flycast/*.patch; do git -C "$B/flycast-src" apply "$p"; done
  echo "$fly_stamp" > "$B/flycast-src/STAMP"
fi
log "Flycast"
cmake -S "$B/flycast-src" -B "$B/flycast" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUSE_VULKAN=OFF -DUSE_LUA=OFF -DUSE_BREAKPAD=OFF -DUSE_HOST_SDL=ON -DUSE_OPENMP=OFF \
  -DUSE_ALSA=OFF -DUSE_LIBAO=OFF -DUSE_PULSEAUDIO=OFF > "$B/flycast-cmake.log"
ninja -C "$B/flycast" -j"$J" flycast > "$B/flycast.log" 2>&1 || { tail -40 "$B/flycast.log"; exit 1; }

# 6. the Dreamcast program
log "Dreamcast program"
rm -rf "$B/guest" && cp -R "$D/guest" "$B/guest"
(cd "$B/guest" && set +u && . "$B/kos/environ.sh" && make SQLITE_DIR="$B/sqlite" > "$B/guest.log" 2>&1) || { tail -40 "$B/guest.log"; exit 1; }
cp "$B/guest/dreamcast-sql.elf" "$B/dreamcast-sql.elf"
log "done: $B/dreamcast-sql.elf"
