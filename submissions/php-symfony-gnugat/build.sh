#!/usr/bin/env bash
# Builds the app as a normal user: production dependencies, compiled .env, warmed-up Symfony cache.
set -euo pipefail
cd "$(dirname "$0")"
export APP_ENV=prod APP_DEBUG=0
# Warming up the cache creates the Doctrine connection service, which needs the env var, but never connects.
export SQLITE_PATH=/nonexistent/feed.db

# FrankenPHP's PHP (see install.sh), not whatever php is on the PATH (GitHub's runner image ships PHP 8.3)
frankenphp-php /usr/local/bin/composer install --no-dev --classmap-authoritative --no-interaction --no-progress
frankenphp-php /usr/local/bin/composer dump-env prod
frankenphp-php bin/console cache:warmup
