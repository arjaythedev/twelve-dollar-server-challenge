#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

SQLITE=sqlite-amalgamation-3540100
SQLITE_SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9

mkdir -p bin deps src/c

if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  curl -fsSL --retry 3 -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  echo "$SQLITE_SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi

common_flags=(
  -O3 -march=native -I "deps/$SQLITE"
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
  -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1
  -DSQLITE_DISABLE_DIRSYNC -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_EXPLAIN
)
tmp_pgo=$(mktemp -d)
trap 'rm -rf "$tmp_pgo"' EXIT

train() {
  local compiler=$1 profile_dir=$2
  local object="$profile_dir/shim.o"
  mkdir -p "$profile_dir"
  # GCC keys counters to the object path. Keep it identical in both passes.
  "$compiler" "${common_flags[@]}" "-fprofile-generate=$profile_dir" -c src/shim.c -o "$object" &&
    "$compiler" "${common_flags[@]}" "-fprofile-generate=$profile_dir" src/c/pgo_train.c \
      "$object" -lpthread -ldl -lm -o "$profile_dir/train" &&
    "$profile_dir/train" "$profile_dir/scratch.db"
}

pgo=()
for compiler in clang gcc; do
  command -v "$compiler" >/dev/null || continue
  profile_dir="$tmp_pgo/$compiler"
  if [[ $compiler == clang ]]; then
    profdata=
    for cmd in llvm-profdata llvm-profdata-{20,19,18,17}; do
      if command -v "$cmd" >/dev/null; then profdata=$cmd; break; fi
    done
    [[ -n $profdata ]] || continue
    train clang "$profile_dir" &&
      "$profdata" merge -output="$profile_dir/merged" "$profile_dir"/*.profraw || continue
    use_profile=("-fprofile-use=$profile_dir/merged")
  else
    train gcc "$profile_dir" || continue
    use_profile=("-fprofile-use=$profile_dir" -fprofile-correction -Werror=missing-profile)
  fi
  if "$compiler" "${common_flags[@]}" "${use_profile[@]}" -fomit-frame-pointer \
      -fno-semantic-interposition -c src/shim.c -o "$profile_dir/shim.o"; then
    mv "$profile_dir/shim.o" src/c/shim_pgo.o
    pgo=(-Dpgo=true)
    echo "Built SQLite with $compiler using the on-box profile"
    break
  fi
done

zig build -Doptimize=ReleaseFast -Dcpu=native "${pgo[@]}"
