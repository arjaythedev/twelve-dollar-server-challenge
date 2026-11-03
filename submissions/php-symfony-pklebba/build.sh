#!/usr/bin/env bash
# Builds the app as a normal user: installs the pinned Composer dependencies (composer.lock) and
# compiles the Symfony container, routes and preload file into var/cache/prod.
set -euo pipefail
cd "$(dirname "$0")"
export APP_ENV=prod APP_DEBUG=0 COMPOSER_NO_INTERACTION=1
composer install --no-dev --classmap-authoritative --no-progress
rm -rf var/cache var/log
mkdir -p var/cache var/log var/caddy
frankenphp php-cli bin/console cache:warmup
