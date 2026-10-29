#!/usr/bin/env bash
# Builds the app as a normal user: production dependencies, compiled .env, warmed-up Symfony cache.
set -euo pipefail
cd "$(dirname "$0")"
export APP_ENV=prod APP_DEBUG=0
# Warming up the cache creates the Doctrine connection service, which needs the env var, but never connects.
export SQLITE_PATH=/nonexistent/feed.db

# php-zts only links itself as php when there's no php yet (GitHub's runner image ships PHP 8.3)
php-zts /usr/local/bin/composer install --no-dev --classmap-authoritative --no-interaction --no-progress
php-zts /usr/local/bin/composer dump-env prod
php-zts bin/console cache:warmup
