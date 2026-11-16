#!/usr/bin/env bash
# Builds bin/server as a normal user. Downloads two pinned sources (checked against SHA-256), builds liburing
# as a static library, compiles the SQLite amalgamation into the binary, then compiles src/*.c.
set -euo pipefail
cd "$(dirname "$0")"
DEPS="$PWD/deps"
SELF="$PWD/build.sh"
mkdir -p "$DEPS" bin

# A plain `bash build.sh` (EXP_CFLAGS unset) builds the entry as submitted: the checkpoint helper process
# (V_CKPT_PROC), multishot recv on a provided buffer ring (V_MULTISHOT_RECV), and a profile-guided gcc build that
# trains itself. Step 1 builds an instrumented server, step 2 (train/train.sh) runs it on a scratch database under a
# curl load for about half a minute and stops it, which writes pgo/*.gcda, step 3 rebuilds with -fprofile-use. If a
# tool is missing or any step fails, the build falls back to the same flags without profile guidance, so a build
# never fails because of training. The experiment harness always sets EXP_CFLAGS (empty means the round 1 baseline).
DEFAULT_FLAGS="-DV_CKPT_PROC -DV_MULTISHOT_RECV -DV_JSON_SPEC"
if [ "${EXP_CFLAGS+set}" != set ]; then
  if command -v curl > /dev/null && command -v sqlite3 > /dev/null && command -v openssl > /dev/null; then
    echo "build: profile-guided build, step 1 of 3 (instrumented server)"
    if EXP_CFLAGS="$DEFAULT_FLAGS -DV_PGO_GEN" bash "$SELF" &&
       echo "build: step 2 of 3 (training load)" && bash "$PWD/train/train.sh" &&
       echo "build: step 3 of 3 (rebuild with the profile)" && EXP_CFLAGS="$DEFAULT_FLAGS -DV_PGO_USE" bash "$SELF"; then
      exit 0
    fi
    echo "build: the profile-guided build failed, building without it"
  else
    echo "build: curl, sqlite3 or openssl is missing, building without the profile"
  fi
  EXP_CFLAGS="$DEFAULT_FLAGS" exec bash "$SELF"
fi

SQLITE_VER=3500400
SQLITE_TGZ="sqlite-autoconf-$SQLITE_VER.tar.gz"
SQLITE_URL="https://www.sqlite.org/2025/$SQLITE_TGZ"
SQLITE_SHA256=a3db587a1b92ee5ddac2f66b3edb41b26f9c867275782d46c3a088977d6a5b18

URING_VER=2.5
URING_TGZ="liburing-$URING_VER.tar.gz"
URING_URL="https://github.com/axboe/liburing/archive/refs/tags/liburing-$URING_VER.tar.gz"
URING_SHA256=456f5f882165630f0dc7b75e8fd53bd01a955d5d4720729b4323097e6e9f2a98

fetch() { # fetch <url> <file> <sha256>
  if [ ! -f "$DEPS/$2" ]; then
    curl -fsSL --retry 3 -o "$DEPS/$2.part" "$1"
    mv "$DEPS/$2.part" "$DEPS/$2"
  fi
  echo "$3  $DEPS/$2" | sha256sum -c --quiet -
}
fetch "$SQLITE_URL" "$SQLITE_TGZ" "$SQLITE_SHA256"
fetch "$URING_URL" "$URING_TGZ" "$URING_SHA256"

if [ ! -f "$DEPS/liburing/src/liburing.a" ]; then
  rm -rf "$DEPS/liburing"; mkdir -p "$DEPS/liburing"
  tar -xzf "$DEPS/$URING_TGZ" -C "$DEPS/liburing" --strip-components=1
  (cd "$DEPS/liburing" && ./configure >/dev/null && make -C src -j"$(nproc)" liburing.a >/dev/null)
fi

# Experiments only (exp/README.md): EXP_CFLAGS carries the -DV_* flags. Unset, this script builds exactly as before.
# V_SQLITE_HEAP also needs the SQLite amalgamation built with memsys5, kept in its own object file.
EXP_CFLAGS="${EXP_CFLAGS:-}"
SQLITE_EXTRA=""
case " $EXP_CFLAGS " in *" -DV_SQLITE_HEAP"*) SQLITE_EXTRA="-DSQLITE_ENABLE_MEMSYS5";; esac
SQLITE_O="$DEPS/sqlite3.o"
[ -n "$SQLITE_EXTRA" ] && SQLITE_O="$DEPS/sqlite3-memsys5.o"

# Round 3. V_PGO_GEN builds the entry and SQLite with -fprofile-generate (the server writes pgo/*.gcda when it gets
# SIGTERM); V_PGO_USE rebuilds both with -fprofile-use from those files. V_BOLT_RELOC keeps relocations in the binary
# for llvm-bolt. SQLite goes in its own object, rebuilt for each mode, so both sides see identical object names.
PGO_FLAGS=""; LINK_EXTRA=""
case " $EXP_CFLAGS " in
  *" -DV_PGO_GEN"*) PGO_FLAGS="-fprofile-generate -fprofile-update=single -fprofile-dir=$PWD/pgo -Wno-coverage-mismatch";;
  *" -DV_PGO_USE"*) PGO_FLAGS="-fprofile-use -fprofile-partial-training -fprofile-dir=$PWD/pgo -Wno-missing-profile -Wno-coverage-mismatch";;
esac
case " $EXP_CFLAGS " in *" -DV_BOLT_RELOC"*) LINK_EXTRA="-Wl,--emit-relocs";; esac
if [ -n "$PGO_FLAGS" ]; then
  mkdir -p "$PWD/pgo"
  SQLITE_O="$DEPS/sqlite3-pgo.o"
  rm -f "$SQLITE_O"
fi

if [ ! -f "$SQLITE_O" ]; then
  if [ ! -f "$DEPS/sqlite/sqlite3.c" ]; then
    rm -rf "$DEPS/sqlite"; mkdir -p "$DEPS/sqlite"
    tar -xzf "$DEPS/$SQLITE_TGZ" -C "$DEPS/sqlite" --strip-components=1
  fi
  gcc -O2 -c -o "$SQLITE_O" "$DEPS/sqlite/sqlite3.c" \
    -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_USE_ALLOCA $SQLITE_EXTRA $PGO_FLAGS
fi

# shellcheck disable=SC2086
gcc -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter $EXP_CFLAGS $PGO_FLAGS $LINK_EXTRA -o bin/server \
  -I"$DEPS/sqlite" -I"$DEPS/liburing/src/include" \
  src/main.c src/app.c src/json.c src/sha256.c "$SQLITE_O" "$DEPS/liburing/src/liburing.a" -lm
echo "built bin/server"
