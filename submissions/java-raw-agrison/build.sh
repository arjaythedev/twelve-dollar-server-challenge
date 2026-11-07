#!/usr/bin/env bash
# Builds as a normal user: compiles SQLite 3.54.0 from the pinned amalgamation into a shared
# library, then compiles the server with javac. No package manager, no dependency but the JDK.
set -euo pipefail
cd "$(dirname "$0")"

SQLITE_BASE="sqlite-amalgamation-3540000"                     # SQLite 3.54.0
SQLITE_URL="https://www.sqlite.org/2026/${SQLITE_BASE}.zip"
SQLITE_SHA256="68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d"

# The benchmark box has the pinned JDK 27 from install.sh. JDK_HOME overrides it (local dev).
JDK_HOME="${JDK_HOME:-/opt/jdk-27}"
if [ ! -x "$JDK_HOME/bin/javac" ] && [ -n "${JAVA_HOME:-}" ]; then JDK_HOME="$JAVA_HOME"; fi
if [ ! -x "$JDK_HOME/bin/javac" ] && command -v javac >/dev/null; then
  JDK_HOME="$(cd "$(dirname "$(dirname "$(command -v javac)")")" && pwd)"
fi
JAVAC="$JDK_HOME/bin/javac"
[ -x "$JAVAC" ] || { echo "no javac: expected /opt/jdk-27 (install.sh), \$JDK_HOME or \$JAVA_HOME"; exit 1; }

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
case "$(uname -m)" in x86_64|amd64) ARCH_FLAG="-march=native";; *) ARCH_FLAG="-mcpu=native";; esac
case "$(uname -s)" in
  Darwin) LIB="native/libsqlite3.dylib"; LINK_FLAGS=(-dynamiclib);;
  *)      LIB="native/libsqlite3.so";    LINK_FLAGS=(-shared -fPIC -fno-plt -fno-semantic-interposition);;
esac

mkdir -p native out
if [ ! -f "$LIB" ]; then
  curl -fsSL "$SQLITE_URL" -o "native/${SQLITE_BASE}.zip"
  echo "$SQLITE_SHA256  native/${SQLITE_BASE}.zip" | sha256 -c -
  unzip -oq "native/${SQLITE_BASE}.zip" -d native
  # THREADSAFE=2: one connection per thread at a time, no per-call mutexes.
  # Everything else trims code we never use or costs we never want to pay.
  cc -O2 "$ARCH_FLAG" -DNDEBUG "${LINK_FLAGS[@]}" \
    -DSQLITE_THREADSAFE=2 \
    -DSQLITE_DQS=0 \
    -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_MAX_EXPR_DEPTH=0 \
    -DSQLITE_OMIT_DEPRECATED \
    -DSQLITE_OMIT_PROGRESS_CALLBACK \
    -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_OMIT_AUTOINIT \
    -DSQLITE_USE_ALLOCA \
    -DSQLITE_MAX_MMAP_SIZE=1073741824 \
    -DSQLITE_ENABLE_MATH_FUNCTIONS \
    -o "$LIB" "native/${SQLITE_BASE}/sqlite3.c" -lpthread -lm
fi

RELEASE=()
JAVAC_MAJOR="$("$JAVAC" -version 2>&1 | awk '{print $2}' | cut -d. -f1)"
[ "${JAVAC_MAJOR:-0}" -ge 27 ] 2>/dev/null && RELEASE=(--release 27)
rm -rf out && mkdir out
# shellcheck disable=SC2046
"$JAVAC" "${RELEASE[@]}" -proc:none -g:none -Xlint:-this-escape -d out $(find src -name '*.java')
echo "built: $LIB + out/ (javac ${JAVAC_MAJOR}${RELEASE+ --release 27})"
