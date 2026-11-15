#!/usr/bin/env bash
# build_remote.sh SRC_DIR TAG [normal|stats|asan] — runs on the benchmark host (not part of the
# submission's build). Same steps as build.sh: Lean checks the specification and derives
# app_gen.c, which must equal the checked-in gen/app_gen.c; then one gcc translation unit with
# build.sh's flags. Lean and SQLite are the pinned releases (Lean under the study root, not /opt).
set -euo pipefail
src=$(cd "${1:?source dir}" && pwd); tag=${2:?tag}; mode=${3:-normal}
R=${STUDY_ROOT:-/root/twelve/lean-153-improve}
LEAN=$R/toolchain/lean-4.35.0/bin
SQL=/root/twelve/deps/sqlite-amalgamation-3540100
echo "0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9  $SQL.zip" | sha256sum -c --quiet -
cd "$src"
mkdir -p build
(cd spec && "$LEAN/lake" build >/dev/null && "$LEAN/lake" exe derive ../build >/dev/null)
for f in app_gen.c transport_gen.h; do
  [ -f "gen/$f" ] || [ "$f" != transport_gen.h ] || continue   # the baseline has no transport header
  if ! cmp -s "build/$f" "gen/$f"; then echo "REGEN MISMATCH $f"; diff -u "gen/$f" "build/$f" | head -40; exit 1; fi
done
flags=(-O2 -march=native -fno-plt -pthread
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  -Iruntime -Ibuild -I"$SQL")
# MODE: "normal" (the production flags only) or '+'-separated test features:
#   stats  -DSTATS counters;  asan  -O1 ASan/UBSan;  fault  -DFAULT_ENTER (enter fault injector);
#   sq4/sq8  a 4/8-entry SQ (the backlog carries most submissions);
#   nbuf4  4 receive buffers per ring;  tight  NBUF 4, CQE_BUDGET 4, MAX_JOBS 64 (the Sizing
#   proofs still hold: 4 = 2^2 <= 32768, 16 * 4 <= 64).
hdr=build
if [ "$mode" != normal ]; then
  IFS=+ read -ra feats <<< "$mode"
  for f in "${feats[@]}"; do
    case "$f" in
      stats) flags+=(-DSTATS) ;;
      asan) flags=("${flags[@]/-O2/-O1}" -g -fsanitize=address,undefined -fno-omit-frame-pointer) ;;
      fault) flags+=(-DFAULT_ENTER) ;;
      pbufresv) flags+=(-DFAULT_PBUF_RESV) ;;            # emulate kernels that refuse resv[0]=0
      sq4) flags+=(-DSQ_ENTRIES=4) ;;
      sq8) flags+=(-DSQ_ENTRIES=8) ;;
      coal1000) flags+=(-DCOALESCE_US=1000) ;;          # ablation: 1 ms coalescing window
      coal500) flags+=(-DCOALESCE_US=500) ;;
      rings5) flags+=(-DWANT_CONNS=320000) ;;           # ablation only: 5 rings (capacity 327k)
      o3) flags=("${flags[@]/-O2/-O3}") ;;
      nbuf4|tight)
        hdr=build-$f; mkdir -p "$hdr"
        sed -e 's/^#define NBUF 1024$/#define NBUF 4/' build/transport_gen.h > "$hdr/transport_gen.h"
        [ "$f" = tight ] && sed -i -e 's/^#define CQE_BUDGET 2048$/#define CQE_BUDGET 4/' -e 's/^#define MAX_JOBS 65536$/#define MAX_JOBS 64/' "$hdr/transport_gen.h"
        grep -q '^#define NBUF 4$' "$hdr/transport_gen.h"
        flags=(-I"$hdr" "${flags[@]}") ;;
      *) echo "unknown feature $f"; exit 1 ;;
    esac
  done
fi
out=$R/artifacts/$tag
mkdir -p "$out"
gcc "${flags[@]}" -o "$out/server" runtime/server.c -lm
( cd "$out" && sha256sum server > binaries.sha256 )
printf '%s\n' "${flags[@]}" > "$out/flags.txt"
{ gcc --version | head -1; "$LEAN/lean" --version; uname -r; } > "$out/toolchain.txt"
sha256sum gen/* build/*_gen.* > "$out/gen.sha256"
cat "$out/binaries.sha256"
echo "BUILD_OK $tag $mode regen=equal"
