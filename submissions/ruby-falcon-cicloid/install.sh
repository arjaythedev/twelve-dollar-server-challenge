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

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fetch() { # fetch <url> <sha256> <file>
  curl -fsSL --retry 3 "$1" -o "$work/$3"
  echo "$2  $work/$3" | sha256sum -c -
}

# SQLite: static library, so the extralite extension embeds it. Options from sqlite.org's
# "recommended compile-time options"; -march=native is fine because it is built on the box.
SQLITE=3540000
if [ ! -f /opt/sqlite/lib/libsqlite3.a ] || ! grep -q "$SQLITE" /opt/sqlite/VERSION 2>/dev/null; then
  fetch "https://sqlite.org/2026/sqlite-amalgamation-$SQLITE.zip" \
    68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d sqlite.zip
  (cd "$work" && unzip -q sqlite.zip)
  src="$work/sqlite-amalgamation-$SQLITE"
  gcc -c -O3 -march=native -fPIC -o "$src/sqlite3.o" "$src/sqlite3.c" \
    -DSQLITE_DQS=0 -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 \
    -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA \
    -DSQLITE_ENABLE_COLUMN_METADATA
  rm -rf /opt/sqlite && mkdir -p /opt/sqlite/lib /opt/sqlite/include
  ar rcs /opt/sqlite/lib/libsqlite3.a "$src/sqlite3.o"
  cp "$src/sqlite3.h" "$src/sqlite3ext.h" /opt/sqlite/include/
  echo "$SQLITE" > /opt/sqlite/VERSION
fi

# libsodium: HMAC-SHA256 for JWT verification, loaded by rbnacl through FFI.
SODIUM=1.0.22
if ! grep -q "$SODIUM" /usr/local/lib/pkgconfig/libsodium.pc 2>/dev/null; then
  fetch "https://download.libsodium.org/libsodium/releases/libsodium-$SODIUM.tar.gz" \
    adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349 sodium.tar.gz
  tar -xzf "$work/sodium.tar.gz" -C "$work"
  (cd "$work/libsodium-$SODIUM" && CFLAGS="-O3 -march=native" ./configure --disable-static \
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
  ./configure --prefix=/opt/ruby --enable-yjit --with-jemalloc --disable-install-doc --disable-install-rdoc
  make -j"$(nproc)"
  make install
fi
/opt/ruby/bin/ruby --yjit -e 'abort "YJIT unavailable" unless RubyVM::YJIT.enabled?; puts RUBY_DESCRIPTION'
