#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends php8.3-cli php8.3-sqlite3 php8.3-mbstring php8.3-intl php8.3-opcache curl ca-certificates unzip
COMPOSER_VERSION=2.8.12
curl -fsSL "https://getcomposer.org/download/$COMPOSER_VERSION/composer.phar" -o /usr/local/bin/composer
printf '%s  /usr/local/bin/composer\n' 'f446ea719708bb85fcbf4ef18def5d0515f1f9b4d703f6d820c9c1656e10a2f2' | sha256sum -c -
chmod +x /usr/local/bin/composer

# Install the pinned runtime; application code and Composer dependencies build on the box.
FRANKENPHP_VERSION=1.13.0
FRANKENPHP_SHA256=05675ca4cda0e50d6d40318b1f7b145572a817666870fa56a41ffff3fdb2425a
curl -fsSL "https://github.com/php/frankenphp/releases/download/v${FRANKENPHP_VERSION}/frankenphp-linux-x86_64-gnu" -o /usr/local/bin/frankenphp
printf '%s  /usr/local/bin/frankenphp\n' "$FRANKENPHP_SHA256" | sha256sum -c -
chmod +x /usr/local/bin/frankenphp
