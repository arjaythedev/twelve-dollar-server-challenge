#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
bash fetch-deps.sh
mkdir -p build bin
CC=${CC:-gcc}
uring=deps/liburing-2.5
mode=${BUILD_MODE:-linked}
store_mode=${STORE_MODE:-3}
sqlite_mutexes=${SQLITE_MUTEXES:-2}
portable=${PORTABLE:-0}
case "$mode" in linked|modules) ;; *) echo 'BUILD_MODE must be linked or modules' >&2; exit 2 ;; esac
case "$store_mode" in 0|1|2|3|4) ;; *) exit 2 ;; esac
case "$sqlite_mutexes" in 0|2) ;; *) exit 2 ;; esac
case "$portable" in 0|1) ;; *) exit 2 ;; esac
flags=(-std=gnu11 -O2 -g -fPIC -fno-plt -pthread -ffunction-sections -fdata-sections
  -Wall -Wextra -Wshadow -Wconversion -Wno-deprecated-declarations
  -Isrc -Ideps -Ideps/sqlite-amalgamation-3540100 -isystem "$uring/src/include")
link=(-Wl,-z,relro,-z,now,--gc-sections)
if [[ "$portable" == 1 ]]; then flags+=(-DASC_PORTABLE=1); fi
if [[ ${SANITIZE:-0} == 1 ]]; then
  flags+=(-O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
  link+=(-fsanitize=address,undefined)
else
  flags+=(-flto);link+=(-flto)
fi
# Each SQLite connection has one owner; checkpoints use a separate connection.
sqlite_flags=(-DSQLITE_THREADSAFE="$sqlite_mutexes" -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_DQS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK
  -DSQLITE_OMIT_SHARED_CACHE)
json_flags=(-DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_UTILS=1)
make -C "$uring/src" clean
(
  cd "$uring"
  ./configure --cc="$CC" --use-libc
  make -C src CC="$CC" LIBURING_CFLAGS='-fPIC -ffunction-sections -fdata-sections' liburing.a
)
uring_library="$uring/src/liburing.a"
tree_object=build/tree.o
json_object=build/yyjson.o
"$CC" "${flags[@]}" -w "${sqlite_flags[@]}" -c src/tree.c -o "$tree_object"
"$CC" "${flags[@]}" -w "${json_flags[@]}" -c deps/yyjson.c -o "$json_object"
generation=build
if [[ "$mode" == modules ]]; then
  mkdir -p bin/generations
  generation=$(mktemp -d "$PWD/bin/generations/gen.XXXXXXXX")
  trap 'rm -rf "$generation"' ERR
fi
objects=()
for module in math codec auth store app transport; do
  definitions=()
  extra=()
  case "$module" in
    codec) definitions+=("${json_flags[@]}");extra=("$json_object" -lm) ;;
    auth) extra=(-lcrypto) ;;
    store) definitions+=(-DASC_STORE_MODE="$store_mode");extra=("$tree_object" -pthread -lm -ldl) ;;
    transport) extra=("$uring_library") ;;
  esac
  if [[ "$mode" == linked ]]; then
    definitions+=(-DASC_MODULE_ENTRY="asc_${module}_module")
    object="$generation/$module.o"
    "$CC" "${flags[@]}" "${definitions[@]}" -c "src/$module.c" -o "$object"
    objects+=("$object")
  else
    "$CC" "${flags[@]}" "${definitions[@]}" -shared "src/$module.c" "${extra[@]}" "${link[@]}" -Wl,-Bsymbolic-functions -o "$generation/$module.so"
  fi
done
if [[ "$mode" == linked ]]; then
  "$CC" "${flags[@]}" -DASC_BUILTIN=1 src/main.c "${objects[@]}" \
    "$tree_object" "$json_object" "$uring_library" -lcrypto -lm -pthread "${link[@]}" -o bin/server.new
else
  "$CC" "${flags[@]}" src/main.c -ldl "${link[@]}" -o bin/server.new
fi
if [[ "$mode" == modules ]]; then
  if [[ -d bin/modules && ! -L bin/modules ]]; then rmdir bin/modules; fi
  ln -s "generations/$(basename "$generation")" bin/modules.new
  mv -Tf bin/modules.new bin/modules
else
  if [[ -L bin/modules ]]; then rm bin/modules; fi
fi
mv bin/server.new bin/server
echo 'Built bin/server'
