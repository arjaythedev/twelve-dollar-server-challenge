#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: PHP 8.3 (Ubuntu's own packages), the OpenSwoole 26.2.0
# extension built from its source tarball, and Composer 2.11.1 (both sha256-pinned). OpenSwoole
# isn't enabled globally; start.sh loads it with `-d extension=openswoole`.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl unzip build-essential autoconf pkg-config \
  php8.3-cli php8.3-dev php8.3-sqlite3 php8.3-mbstring php8.3-opcache

OPENSWOOLE_VERSION=26.2.0
OPENSWOOLE_SHA256=0dd86480a3e4f6ad5474bbdf441facc656dba668adceab93a35dbfe2e4d57e70
COMPOSER_VERSION=2.11.1
COMPOSER_SHA256=6f1b891e1d4927d62740276c540fb490e47689e19e102c49e8da849794882f8c

tmp="$(mktemp -d)"
curl -fsSL -o "$tmp/openswoole.tar.gz" "https://github.com/openswoole/ext-openswoole/archive/refs/tags/v$OPENSWOOLE_VERSION.tar.gz"
echo "$OPENSWOOLE_SHA256  $tmp/openswoole.tar.gz" | sha256sum -c -
tar -xzf "$tmp/openswoole.tar.gz" -C "$tmp"
cd "$tmp/ext-openswoole-$OPENSWOOLE_VERSION"
phpize
./configure   # only the HTTP server is used: no OpenSSL, HTTP/2, curl or Postgres hooks
make -j"$(nproc)"
make install
cd /

curl -fsSL -o /usr/local/bin/composer "https://getcomposer.org/download/$COMPOSER_VERSION/composer.phar"
echo "$COMPOSER_SHA256  /usr/local/bin/composer" | sha256sum -c -
chmod 0755 /usr/local/bin/composer
rm -rf "$tmp"

php -d extension=openswoole -r 'echo "PHP ", PHP_VERSION, ", OpenSwoole ", phpversion("openswoole"), ", SQLite ", (new PDO("sqlite::memory:"))->query("select sqlite_version()")->fetchColumn(), "\n";'
