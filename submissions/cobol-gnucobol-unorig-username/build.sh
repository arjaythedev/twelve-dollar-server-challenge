#!/usr/bin/env bash
# Builds bin/server: server.cob compiled by GnuCOBOL 3.2, linked with SQLite 3.54.1 (official
# amalgamation, SHA-256 pinned) and libcrypto (used for SHA-256 only).
set -euo pipefail
cd "$(dirname "$0")"
SQLITE=sqlite-amalgamation-3540100
SHA=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
mkdir -p bin deps
if [ ! -f deps/$SQLITE/sqlite3.c ]; then
  curl -fsSL -o deps/$SQLITE.zip https://www.sqlite.org/2026/$SQLITE.zip
  echo "$SHA  deps/$SQLITE.zip" | sha256sum -c -
  unzip -qo deps/$SQLITE.zip -d deps
fi
gcc -O2 -march=x86-64-v2 -w -c -o deps/sqlite3.o deps/$SQLITE/sqlite3.c \
  -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED \
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA -DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1
# CALL STATIC targets get their real C prototypes, so pointer and 64-bit returns keep their types.
HDRS="-include stdlib.h -include string.h -include unistd.h -include time.h -include signal.h \
 -include sys/socket.h -include sys/epoll.h -include sys/resource.h -include arpa/inet.h -include errno.h \
 -include fcntl.h -include sys/prctl.h -include openssl/sha.h -include $PWD/deps/$SQLITE/sqlite3.h"
cobc -x -free -O2 -fstatic-call -fno-gen-c-decl-static-call -fnotrunc \
  -A "-march=x86-64-v2 -w $HDRS" \
  -o bin/server server.cob deps/sqlite3.o -lcrypto
