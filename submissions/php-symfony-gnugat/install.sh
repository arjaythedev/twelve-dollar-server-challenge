#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs FrankenPHP and PHP 8.5 (ZTS) from the deb packages
# documented in FrankenPHP's README, and Composer.
set -euo pipefail

# Upstream versions are pinned, package revisions are not: the repository replaces a revision when it rebuilds
# (8.5.11-2 became 8.5.11-3), so apt picks the highest revision available.
FRANKENPHP_VERSION=1.13.0+php85-*
PHP_VERSION=8.5.11-*
PHP_EXT_VERSION=8.5.11-*
COMPOSER_VERSION=2.10.2
COMPOSER_SHA256=5ee7125f8a30a34d246cefdc0bc85b8a783b28f2aec968994118512350d28027

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl unzip

mkdir -p /etc/apt/keyrings
curl -fsSL https://pkg.henderkes.com/api/packages/85/debian/repository.key -o /etc/apt/keyrings/static-php85.asc
echo "deb [signed-by=/etc/apt/keyrings/static-php85.asc] https://pkg.henderkes.com/api/packages/85/debian php-zts main" \
  > /etc/apt/sources.list.d/static-php85.list
apt-get update
apt-get install -y --no-install-recommends \
  "frankenphp=${FRANKENPHP_VERSION}" \
  "php-zts-cli=${PHP_VERSION}" \
  "php-zts-embed=${PHP_VERSION}" \
  "php-zts-pdo=${PHP_EXT_VERSION}" \
  "php-zts-pdo-sqlite=${PHP_EXT_VERSION}" \
  "php-zts-sqlite3=${PHP_EXT_VERSION}"

curl -fsSL -o /usr/local/bin/composer "https://getcomposer.org/download/${COMPOSER_VERSION}/composer.phar"
echo "${COMPOSER_SHA256}  /usr/local/bin/composer" | sha256sum -c -
chmod +x /usr/local/bin/composer
