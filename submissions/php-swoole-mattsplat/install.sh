#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs PHP 8.3 (Ubuntu's own packages) and builds the
# Swoole 6.2.3 extension from its source tarball (checksum pinned) into PHP's extension directory.
# The extension is not enabled globally; start.sh loads it with `-d extension=swoole`.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl build-essential autoconf pkg-config \
  php8.3-cli php8.3-dev php8.3-sqlite3 php8.3-mbstring php8.3-opcache

VERSION=6.2.3
SHA256=dde8d2a4a6b5c5cd418aedd8561760baad59767a2f8a963b2c0e9eb9c86f4c8d
tmp="$(mktemp -d)"
curl -fsSL -o "$tmp/swoole.tar.gz" "https://github.com/swoole/swoole-src/archive/refs/tags/v$VERSION.tar.gz"
echo "$SHA256  $tmp/swoole.tar.gz" | sha256sum -c -
tar -xzf "$tmp/swoole.tar.gz" -C "$tmp"
cd "$tmp/swoole-src-$VERSION"
phpize
# Only the HTTP server is used: no OpenSSL, HTTP/2 client libs, curl/sqlite coroutine hooks or brotli.
./configure --disable-brotli --disable-zstd
make -j"$(nproc)"
make install
cd /
rm -rf "$tmp"
php -d extension=swoole -r 'echo "PHP ", PHP_VERSION, ", Swoole ", phpversion("swoole"), ", SQLite ", (new PDO("sqlite::memory:"))->query("select sqlite_version()")->fetchColumn(), "\n";'
