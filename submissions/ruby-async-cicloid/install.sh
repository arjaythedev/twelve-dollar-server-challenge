#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Builds from pinned, checksummed sources:
#   Ruby 4.0 (YJIT, jemalloc) under /opt/ruby, SQLite 3.54 (static, tuned) under /opt/sqlite,
#   libsodium 1.0.22 under /usr/local.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates pkg-config \
  libssl-dev libyaml-dev zlib1g-dev libffi-dev libgmp-dev libjemalloc-dev rustc \
  liburing-dev unzip

here="$(cd "$(dirname "$0")" && pwd)"
# Tune for the CPU it is built on (the benchmark droplet is x86_64). On arm64 -march=native combines
# with Ruby's pointer-authentication flags into binaries that crash under some VMs, so skip it there.
NATIVE=""
[ "$(uname -m)" = x86_64 ] && NATIVE="-march=native"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fetch() { # fetch <url> <sha256> <file>
  curl -fsSL --retry 3 "$1" -o "$work/$3"
  echo "$2  $work/$3" | sha256sum -c -
}

# SQLite: static library, so the extralite extension embeds it. Options from sqlite.org's
# "recommended compile-time options".
# Profile-guided: an instrumented build runs sqlite_train.c (the server's statements, prepared
# once, in the load test's mix) and the final build uses that profile.
SQLITE=3540000
SQLITE_FLAGS="-O3 $NATIVE -fPIC -DSQLITE_DQS=0 -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA -DSQLITE_ENABLE_COLUMN_METADATA"
if ! grep -qx "$SQLITE-pgo" /opt/sqlite/VERSION 2>/dev/null; then
  fetch "https://sqlite.org/2026/sqlite-amalgamation-$SQLITE.zip" \
    68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d sqlite.zip
  (cd "$work" && unzip -q sqlite.zip)
  src="$work/sqlite-amalgamation-$SQLITE"
  # The profile is keyed by the object's output path, so both builds write "$work/sqlite3.o".
  pgo="-fprofile-dir=$work/pgo"
  # shellcheck disable=SC2086
  gcc $SQLITE_FLAGS -fprofile-generate -fprofile-update=single "$pgo" -c -o "$work/sqlite3.o" "$src/sqlite3.c"
  gcc -O2 -I"$src" -o "$work/sqlite_train" "$here/sqlite_train.c" "$work/sqlite3.o" -fprofile-generate -lm
  "$work/sqlite_train" "$work/train.db"
  # shellcheck disable=SC2086
  gcc $SQLITE_FLAGS -fprofile-use -fprofile-partial-training "$pgo" -Werror=missing-profile \
    -c -o "$work/sqlite3.o" "$src/sqlite3.c"
  rm -rf /opt/sqlite && mkdir -p /opt/sqlite/lib /opt/sqlite/include
  ar rcs /opt/sqlite/lib/libsqlite3.a "$work/sqlite3.o"
  cp "$src/sqlite3.h" "$src/sqlite3ext.h" /opt/sqlite/include/
  echo "$SQLITE-pgo" > /opt/sqlite/VERSION
fi

# libsodium: HMAC-SHA256 for JWT verification, loaded by rbnacl through FFI.
SODIUM=1.0.22
if ! grep -q "$SODIUM" /usr/local/lib/pkgconfig/libsodium.pc 2>/dev/null; then
  fetch "https://download.libsodium.org/libsodium/releases/libsodium-$SODIUM.tar.gz" \
    adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349 sodium.tar.gz
  tar -xzf "$work/sodium.tar.gz" -C "$work"
  (cd "$work/libsodium-$SODIUM" && CFLAGS="-O3 $NATIVE" ./configure --disable-static \
    && make -j"$(nproc)" && make install)
  ldconfig
fi

RUBY_VERSION=4.0.7
RUBY_SHA256=911ace20f90d068ca0e4dda6d0e4f0f81e52e52f2dd4f4004c721e253412e82d
if [ "$(/opt/ruby/bin/ruby -e 'print RUBY_VERSION' 2>/dev/null)" != "$RUBY_VERSION" ]; then
  fetch "https://cache.ruby-lang.org/pub/ruby/4.0/ruby-$RUBY_VERSION.tar.gz" "$RUBY_SHA256" ruby.tar.gz
  cd "$work"
  tar -xzf ruby.tar.gz
  cd "ruby-$RUBY_VERSION"
  ./configure --prefix=/opt/ruby --enable-yjit --with-jemalloc --disable-install-doc --disable-install-rdoc \
    cflags="$NATIVE"
  make -j"$(nproc)"
  make install
fi
/opt/ruby/bin/ruby --yjit -e 'abort "YJIT unavailable" unless RubyVM::YJIT.enabled?; puts RUBY_DESCRIPTION'
