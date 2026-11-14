#!/usr/bin/env bash
# One source mirror. Artifact suffixes identify compiler/test modes, not divergent sources.
set -euo pipefail
sha=${1:?source commit SHA}
full_sha=${2:?full source commit SHA}
mode=${3:-normal}
[[ "$full_sha" == "$sha"* && ${#full_sha} == 40 ]] || { echo "source SHA mismatch"; exit 1; }
root=/root/twelve/opt-audit-1024/metal
cd "$root/source"
tag=$sha
flags=(-O2 -march=native -fno-plt -pthread -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0
 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
 -Iruntime -Igen -I/root/twelve/deps/sqlite-amalgamation-3540100)
source=runtime/server.c
case "$mode" in
 normal) ;;
 small) tag=$sha-small; flags+=(-DSQ_ENTRIES=4) ;;
 stats) tag=$sha-stats; flags+=(-DSTATS) ;;
 batch) tag=$sha-batch; source=tests/metal/batch_cases.c; flags+=(-O1 -g -fsanitize=address,undefined) ;;
 *) echo "unknown build mode: $mode"; exit 1 ;;
esac
out=$root/artifacts/$tag
mkdir -p "$out"
gcc "${flags[@]}" -o "$out/server" "$source" -lm
binaries=("$out/server")
if [[ "$mode" == normal ]]; then
 gcc "${flags[@]}" -o "$out/micro" tests/metal/micro.c -lm
 binaries+=("$out/micro")
fi
sha256sum "${binaries[@]}" > "$out/binaries.sha256"
printf '%s\n' "$full_sha" > "$out/source.sha"
printf '%s\n' "$mode" > "$out/mode.txt"
printf '%s\n' "${flags[@]}" > "$out/flags.txt"
gcc --version | head -1 > "$out/compiler.txt"
cat "$out/binaries.sha256"
