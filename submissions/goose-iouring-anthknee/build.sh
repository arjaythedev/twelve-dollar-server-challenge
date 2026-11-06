#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
  echo 'The server requires Linux x86_64 for io_uring.' >&2
  exit 1
fi
if [[ $# -gt 1 || ( $# -eq 1 && ${1:-} != --no-pgo ) ]]; then
  echo 'Usage: bash build.sh [--no-pgo]' >&2
  exit 1
fi
pgo=1
if [[ ${1:-} == --no-pgo ]]; then pgo=0; fi
export PATH="/opt/goose-v2026.3:$PATH"
cc=${CC:-gcc-13}
if [[ $pgo == 1 && $("$cc" --version) == *clang* ]]; then
  echo 'This build uses GCC PGO. Select GCC 13 or pass --no-pgo.' >&2
  exit 1
fi
if ! command -v goose >/dev/null || ! goose version | grep -Fq 4d3da949e563; then
  echo 'Goose v2026.3 is required; run install.sh first.' >&2
  exit 1
fi

sqlite_name=sqlite-amalgamation-3540100
sqlite_sha=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p build/vendor
archive="build/vendor/$sqlite_name.zip"
if [[ ! -f $archive ]]; then
  temporary=$(mktemp -d "$PWD/build/vendor/download.XXXXXX")
  trap 'rm -rf -- "$temporary"' EXIT
  curl -fsSL --retry 3 --max-time 180 \
    "https://sqlite.org/2026/$sqlite_name.zip" -o "$temporary/sqlite.zip"
  echo "$sqlite_sha  $temporary/sqlite.zip" | sha256sum -c -
  mv "$temporary/sqlite.zip" "$archive"
fi
echo "$sqlite_sha  $archive" | sha256sum -c -
unzip -q -o "$archive" -d build/vendor
sqlite="build/vendor/$sqlite_name"
out="$PWD/build/variants/goose-300000"
mkdir -p "$out"

common=(-std=gnu11 -O3 -g -fno-omit-frame-pointer -pthread
  -I "$out" -I "$sqlite" -I reference/pavly-c/src
  -fno-plt -fno-semantic-interposition -fomit-frame-pointer -flto
  -march=x86-64-v3 -mtune=cascadelake -mprefer-vector-width=256)
link=(-O3 -pthread -lm -static -flto)
if [[ $("$cc" --version) == *clang* ]]; then link+=(-fuse-ld=lld); fi
sqlite_flags=(-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_OMIT_PROGRESS_CALLBACK
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DSQLITE_DQS=0
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE
  -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_GET_TABLE
  -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_JSON -DSQLITE_TEMP_STORE=3
  -DHAVE_USLEEP=1 -DNDEBUG -DHAVE_FDATASYNC=1)

goose -O2 --standalone --header "$out/goose.h" -o "$out/goose.c" src/goose-300000/server.goose
compile() {
  "$cc" "${common[@]}" "$@" -c "$out/goose.c" -o "$out/goose.o" || return
  "$cc" "${common[@]}" "${sqlite_flags[@]}" "$@" -c src/goose-300000/transport.c -o "$out/native.o" || return
  "$cc" "$@" "$out/native.o" "$out/goose.o" "${link[@]}" -o "$out/server.build"
}

if [[ $pgo == 1 ]]; then
  common+=("-fprofile-prefix-path=$out")
  for attempt in 1 2 3; do
    training=$(mktemp -d "$out/training.XXXXXX")
    profile_dir="$training/pgo"
    mkdir -p "$training/src" "$profile_dir"
    for file in train.sh train.c sha256.h; do cp "reference/pavly-c/src/$file" "$training/src/$file"; done
    # The reference trainer checks shallow paths; GCC may put profiles deeper.
    profile_view=trainer-profile-view
    if [[ $out == "/$profile_view/"* ]]; then profile_view=trainer-profile-view-alt; fi
    ln -s "$profile_dir$out" "$profile_dir/$profile_view"
    compile -fprofile-generate="$profile_dir" -fprofile-update=single
    if CC="$cc" bash "$training/src/train.sh" "$out/server.build" &&
       compile -fprofile-use="$profile_dir" -fprofile-partial-training -Werror=missing-profile; then
      break
    fi
    if [[ $attempt == 3 ]]; then
      echo 'PGO training or profile-use compilation failed after 3 attempts.' >&2
      exit 1
    fi
    echo "PGO attempt $attempt failed; retrying with a fresh profile." >&2
  done
else
  compile
fi
mv "$out/server.build" "$out/server"
cp "$out/server" build/server
echo "Built $out/server"
