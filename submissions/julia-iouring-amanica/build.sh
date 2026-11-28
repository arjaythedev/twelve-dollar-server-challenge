#!/usr/bin/env bash
# Builds as a normal user: the pinned SQLite amalgamation + csrc/shim.c into build/libjr.so.
set -euo pipefail
cd "$(dirname "$0")"
. ./config.sh
DL="${JR_DL_CACHE:-build/dl}"    # download cache (the dev container mounts a volume here)
mkdir -p "$DL" build

# JR_SQLITE_OPT: optimization flags for SQLite + shim (dev A/B only; dev/ct.sh SQLITE_OPT=).
OPT="${JR_SQLITE_OPT:--O2}"
# -fno-semantic-interposition (fsharp-kestrel-delneg): calls between SQLite's exported functions inside
# libjr.so go direct and can be inlined, not through the PLT. ~6% fewer user cycles per request (PLAN 8.17).
CFLAGS="$OPT -fPIC -fno-semantic-interposition -DNDEBUG -DSQLITE_DQS=0 -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_USE_ALLOCA -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1"
# Profile-guided SQLite (PLAN 8.9; idea from fsharp-kestrel-delneg): compile with profiling, run this
# server's statements on a synthetic database (csrc/sqlite-train.c), compile again with the profile.
# JR_PGO=0: one plain compile (dev A/B only; dev/ct.sh PGO=0).
PGO="${JR_PGO:-1}"
stamp="$( { echo "$SQLITE_ID $CFLAGS PGO=$PGO"; cat csrc/shim.c csrc/sqlite-train.c; } | sha256sum | cut -c1-16)"

if [ ! -f build/libjr.so ] || [ "$(cat build/libjr.stamp 2>/dev/null)" != "$stamp" ]; then
  zip="$DL/sqlite-amalgamation-$SQLITE_ID.zip"
  [ -f "$zip" ] || { curl -fsSL -o "$zip.part" "$SQLITE_URL"; mv "$zip.part" "$zip"; }
  got="$(openssl dgst -sha3-256 -r "$zip" | cut -d' ' -f1)"
  [ "$got" = "$SQLITE_SHA3_256" ] || { echo "sqlite checksum mismatch: $got" >&2; rm -f "$zip"; exit 1; }
  rm -rf build/sqlite && mkdir -p build/sqlite && unzip -q -j "$zip" -d build/sqlite
  echo "compiling SQLite $SQLITE_VERSION"
  # shellcheck disable=SC2086
  if [ "$PGO" != 0 ]; then
    # Same object path both times, so the second compile finds the profile (build/pgo/sqlite3.gcda).
    rm -rf build/pgo && mkdir -p build/pgo
    cc $CFLAGS -fprofile-generate -fprofile-update=single -c build/sqlite/sqlite3.c -o build/pgo/sqlite3.o
    cc -O2 -Ibuild/sqlite csrc/sqlite-train.c build/pgo/sqlite3.o -fprofile-generate -o build/pgo/train -lpthread -lm
    build/pgo/train build/pgo/train.db
    cc $CFLAGS -fprofile-use -fprofile-partial-training -Wno-missing-profile -c build/sqlite/sqlite3.c -o build/pgo/sqlite3.o
    cc $CFLAGS -Ibuild/sqlite -shared -o build/libjr.so.tmp build/pgo/sqlite3.o csrc/shim.c -lpthread -lm
    rm -rf build/pgo
  else
    cc $CFLAGS -Ibuild/sqlite -shared -o build/libjr.so.tmp build/sqlite/sqlite3.c csrc/shim.c -lpthread -lm
  fi
  mv build/libjr.so.tmp build/libjr.so
  echo "$stamp" > build/libjr.stamp
fi
"$JULIA_HOME/bin/julia" --version

# Ahead-of-time build (JR_AOT=0: skip it, start.sh then runs src/main.jl on the JIT, for dev A/B).
# Julia's own juliac build script (shipped in the pinned tarball under share/julia/test/trimming; JuliaC.jl
# is the packaged front end to the same script) compiles src/app.jl with --trim: only code reachable
# from main is kept, nothing is compiled at startup. The JR_* dev switches are constants read from ENV,
# so they are part of the stamp. If the build fails, start.sh falls back to the JIT.
if [ "${JR_AOT:-1}" != 0 ]; then
  J="$JULIA_HOME/bin/julia"
  BS="$JULIA_HOME/share/julia/test/trimming/juliac-buildscript.jl"
  astamp="$( { echo "$JULIA_VERSION $stamp"; env | grep '^JR_' | grep -v '^JR_DL_CACHE=' | sort; cat src/*.jl; } | sha256sum | cut -c1-16)"
  if [ ! -x build/server ] || [ "$(cat build/server.stamp 2>/dev/null)" != "$astamp" ]; then
    rm -f build/server build/server.stamp
    echo "compiling the server ahead of time (juliac --trim)"
    # native: built on the box it runs on, like the JIT (the default for an object file is generic x86-64)
    if "$J" -t 1 -O3 --cpu-target="${JR_AOT_CPU:-native}" -J "$JULIA_HOME/lib/julia/sys.so" --startup-file=no --history-file=no \
         --output-o build/server-o.a --output-incremental=no --strip-ir --strip-metadata --experimental --trim=safe \
         "$BS" "$PWD/src/app.jl" --output-exe true > build/aot.log 2>&1; then
      jc() { "$J" --startup-file=no -e 'include(joinpath(Sys.BINDIR, Base.DATAROOTDIR, "julia", "julia-config.jl"))' -- "$@" | tr '\n' ' '; }
      # julia-config quotes its paths for a shell: eval
      eval cc -o build/server.tmp -Wl,--whole-archive build/server-o.a -Wl,--no-whole-archive \
        "$(jc --cflags)" -lm "$(jc --ldflags --ldlibs)" -ljulia-internal >> build/aot.log 2>&1 &&
        mv build/server.tmp build/server && echo "$astamp" > build/server.stamp
    fi
    rm -f build/server-o.a
    [ -x build/server ] || { echo "ahead-of-time build failed (build/aot.log): start.sh will use the JIT" >&2; tail -20 build/aot.log >&2; }
  fi
fi
