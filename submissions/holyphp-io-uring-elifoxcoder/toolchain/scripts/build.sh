#!/usr/bin/env bash
# build.sh — build the hphp toolchain binary.
#
# 1. builds the vendored SQLite once (with src/hp_btree.c appended) into a
#    static library; genembed then embeds it and sqlite3.h into the compiler
# 2. regenerates the embedded runtime header (scripts/genembed.c)
# 3. compiles compiler objects with gcc -c (rock solid)
# 4. links with ld directly — the gcc driver's collect2 intermittently fails
#    on this machine (on-access AV scanners); manual ld always succeeds.
set -u
cd "$(dirname "$0")/.."

CC=${CC:-gcc}
# HPHP_CFLAGS / HPHP_OUT let a caller build a second flavour of the compiler
# next to the normal one (scripts/build-asan.sh uses this for the sanitizer
# build) without changing what the normal build produces.
CFLAGS=${HPHP_CFLAGS:-"-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable"}
OUT=${HPHP_OUT:-hphp.exe}

# 1. build the vendored SQLite static library once per platform/toolchain.
#    The amalgamation is pinned in third_party/sqlite (see its README) and the
#    B-tree driver is appended to it: it uses SQLite's private Btree API.
#    HPHP_SQLITE_H / HPHP_SQLITE_C / HPHP_SQLITE_BTREE / HPHP_SQLITE_CFLAGS are
#    only there for experiments; the default is the vendored tree.
#    The library is embedded into the compiler, so a program links it without
#    a system sqlite3 and without network access.
SQLITE_H=${HPHP_SQLITE_H:-third_party/sqlite/sqlite3.h}
SQLITE_C=${HPHP_SQLITE_C:-third_party/sqlite/sqlite3.c}
SQLITE_BTREE=${HPHP_SQLITE_BTREE:-src/hp_btree.c}
# Per-OS directory: Windows and WSL share this checkout, and their .o files
# are not interchangeable.
SQLITE_PLAT=$(uname -s | tr -c 'A-Za-z0-9._-' '_')
SQLITE_DIR="build/sqlite/$SQLITE_PLAT-$(uname -m)"
SQLITE_LIB="$SQLITE_DIR/libsqlite3.a"
# The options the challenge build used: no unused SQLite features, thread-safe
# (the background checkpoint worker uses its own connection), and the fast
# allocator/time calls. SQLITE_USE_ALLOCA needs the stack limit SQLite expects.
SQLITE_DEFS=(
  SQLITE_THREADSAFE=2
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_OMIT_PROGRESS_CALLBACK
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_AUTOINIT
  SQLITE_OMIT_UTF16
  SQLITE_OMIT_TRACE
  SQLITE_OMIT_COMPLETE
  SQLITE_OMIT_GET_TABLE
  SQLITE_OMIT_TCL_VARIABLE
  SQLITE_USE_ALLOCA
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_DECLTYPE
  SQLITE_OMIT_COMPILEOPTION_DIAGS
  HAVE_FDATASYNC=1
  HAVE_USLEEP=1
)
if [[ ! -f "$SQLITE_H" || ! -f "$SQLITE_C" || ! -f "$SQLITE_BTREE" ]]; then
  echo "sqlite: vendored sources missing ($SQLITE_C, $SQLITE_H, $SQLITE_BTREE)"
  exit 1
fi
# Remember the flags that produced the archive: a change to SQLITE_DEFS or
# HPHP_SQLITE_CFLAGS must rebuild it, even though the sources did not change.
SQLITE_FLAGS_LINE="$CC ${HPHP_SQLITE_CFLAGS:--O3} ${SQLITE_DEFS[*]}"
SQLITE_FLAGS_STAMP="$SQLITE_DIR/flags.txt"
if [[ ! -f "$SQLITE_LIB" || "$SQLITE_C" -nt "$SQLITE_LIB" ||
      "$SQLITE_H" -nt "$SQLITE_LIB" || "$SQLITE_BTREE" -nt "$SQLITE_LIB" ||
      ! -f "$SQLITE_FLAGS_STAMP" ||
      "$(cat "$SQLITE_FLAGS_STAMP" 2>/dev/null)" != "$SQLITE_FLAGS_LINE" ]]; then
  mkdir -p "$SQLITE_DIR" || { echo "sqlite: cannot create $SQLITE_DIR"; exit 1; }
  cat "$SQLITE_C" "$SQLITE_BTREE" > "$SQLITE_DIR/sqlite3_all.c" || exit 1
  sqlite_flags=()
  for def in "${SQLITE_DEFS[@]}"; do sqlite_flags+=("-D$def"); done
  # -O3 and no LTO: the same archive is used for every program, and it must
  # link with the plain gcc driver. With the three feature omits above this
  # measured 4-8% on reads and inserts against -O2 (benchmarks/RESULTS.md);
  # -fno-strict-aliasing is required by SQLite.
  $CC ${HPHP_SQLITE_CFLAGS:--O3} -std=c11 -fno-strict-aliasing \
      -I"$(dirname "$SQLITE_H")" "${sqlite_flags[@]}" \
      -c "$SQLITE_DIR/sqlite3_all.c" -o "$SQLITE_DIR/sqlite3.o" \
    || { echo "sqlite: compile failed"; exit 1; }
  rm -f "$SQLITE_LIB"
  ${AR:-ar} rcs "$SQLITE_LIB" "$SQLITE_DIR/sqlite3.o" \
    || { echo "sqlite: ar failed"; exit 1; }
  printf '%s\n' "$SQLITE_FLAGS_LINE" > "$SQLITE_FLAGS_STAMP" || exit 1
  echo "sqlite: built $(basename "$SQLITE_LIB") ($(stat -c%s "$SQLITE_LIB") bytes)"
fi

# 2. embed the runtime sources into the compiler binary
# (name differs per platform: gcc on MinGW auto-appends .exe, Linux does not;
#  failure is fatal — a stale embed silently breaks the produced binary)
GE=build/genembed
[[ "$(uname -s)" == MINGW* || "$(uname -s)" == *NT* ]] && GE=build/genembed.exe
mkdir -p build
rm -f build/genembed build/genembed.exe
$CC -O2 -std=c11 scripts/genembed.c -o "$GE" || { echo "genembed build failed"; exit 1; }
mkdir -p src/hphpc
# bundled libraries: every package folder in packages/lib/ ships inside the
# compiler, so imports such as "html" work without files on disk.
libs=(packages/lib/*/*.hphp)
[ -e "${libs[0]}" ] || libs=()
"$GE" src/hphpc/runtime_embed.h src/hphpc/runtime "${libs[@]}" \
      --sqlite-h "$SQLITE_H" --sqlite-lib "$SQLITE_LIB" \
      --sqlite-embed src/hphpc/sqlite_embed.h \
  || { echo "embed failed"; exit 1; }

if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == *NT* || -n "${OS:-}" && "${OS:-}" == Windows_NT ]]; then
  # ---- MinGW/Windows: link the PE image directly with ld ----
  OUT=${HPHP_OUT:-hphp.exe}
  GCCLIB=$(gcc -print-file-name=libgcc.a | xargs dirname)
  GCCDIR=$(gcc -print-file-name=crtbegin.o | xargs dirname)
  CRTDIR=$(dirname "$(gcc -print-file-name=libmingwex.a)")

  # 2. compile objects
  mkdir -p build/obj
  objs=()
  rc=0
  for src in src/hphpc/*.c; do
    obj="build/obj/$(basename "${src%.c}").o"
    if ! $CC $CFLAGS -Isrc/hphpc -c "$src" -o "$obj"; then rc=1; fi
    objs+=("$obj")
  done
  [ $rc -eq 0 ] || { echo "compile failed"; exit 1; }

  # 3. link
  rm -f "$OUT"
  ld -m i386pep -Bdynamic -o "$OUT" \
    "$CRTDIR/crt2.o" "$GCCDIR/crtbegin.o" \
    -L"$GCCDIR" -L"$CRTDIR" \
    "${objs[@]}" \
    -lmingw32 -lgcc -lgcc_eh -lmingwex -lmsvcrt -lkernel32 -lpthread -ladvapi32 -lshell32 -luser32 -lws2_32 \
    -lgdi32 -lgdiplus -lcomctl32 -lcomdlg32 -lole32 -loleaut32 -luuid -lversion -lshlwapi -lwinmm \
    "$CRTDIR/default-manifest.o" "$GCCDIR/crtend.o" || { echo "link failed"; exit 1; }

  sig=$(head -c 2 "$OUT" 2>/dev/null | od -An -tx1 | tr -d ' \n')
  [ "$sig" = "4d5a" ] || { echo "output invalid"; exit 1; }
  echo "build ok: $OUT ($(stat -c%s "$OUT") bytes, runtime embedded)"
else
  # ---- POSIX (Linux/macOS): plain gcc link, ELF out ----
  OUT=${HPHP_OUT:-hphp}
  mkdir -p build/obj
  objs=()
  rc=0
  for src in src/hphpc/*.c; do
    obj="build/obj/$(basename "${src%.c}").o"
    if ! $CC $CFLAGS -Isrc/hphpc -c "$src" -o "$obj"; then rc=1; fi
    objs+=("$obj")
  done
  [ $rc -eq 0 ] || { echo "compile failed"; exit 1; }

  rm -f "$OUT"
  # CFLAGS again: -fsanitize=... must also be on the link line (build-asan.sh)
  $CC $CFLAGS "${objs[@]}" -o "$OUT" -lm -lpthread || { echo "link failed"; exit 1; }

  sig=$(head -c 2 "$OUT" 2>/dev/null | od -An -tx1 | tr -d ' \n')
  [ "$sig" = "7f45" ] || { echo "output invalid (not ELF)"; exit 1; }
  echo "build ok: $OUT ($(stat -c%s "$OUT") bytes, runtime embedded)"
fi
