#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
openssl_version=3.5.9
openssl_sha256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
openssl_url="https://github.com/openssl/openssl/releases/download/openssl-$openssl_version/openssl-$openssl_version.tar.gz"
mkdir -p bin .build
archive="$PWD/.build/openssl-$openssl_version.tar.gz"
if [[ ! -f "$archive" ]] || ! printf '%s  %s\n' "$openssl_sha256" "$archive" | sha256sum -c --status; then
  curl -fsSL --retry 3 "$openssl_url" -o "$archive.download"
  printf '%s  %s\n' "$openssl_sha256" "$archive.download" | sha256sum -c -
  mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$openssl_sha256" "$archive" | sha256sum -c -
openssl_dir="$PWD/.build/openssl-$openssl_version"
if [[ ! -f "$openssl_dir/libcrypto.a" ]]; then
  tar -xzf "$archive" -C .build
  (cd "$openssl_dir" && ./config no-shared no-module no-tests && make -j2 build_generated && make -j2 libcrypto.a)
fi
cc -O3 -DNDEBUG -flto -fno-plt -fomit-frame-pointer -march=x86-64-v3 \
  -Wall -Wextra -Wshadow -Wno-unused-function -Wno-unused-variable \
  -Wno-unused-but-set-variable -std=gnu11 -pthread -DRING_WAL_FADVISE_TEST \
  -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_OMIT_PROGRESS_CALLBACK \
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_DEPRECATED \
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA \
  -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 \
  -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE \
  -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_JSON \
  -DSQLITE_TEMP_STORE=3 -DHAVE_FDATASYNC=1 -DHAVE_POSIX_FALLOCATE=1 \
  -DHAVE_USLEEP=1 \
  -Isrc -Isrc/vendor/sqlite-3.54.1 -I"$openssl_dir/include" \
  src/server.c src/ring.c src/wal_vfs.c "$openssl_dir/libcrypto.a" -o bin/server \
  -Wl,-O1 -Wl,--as-needed -ldl -lm -pthread
