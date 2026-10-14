#!/usr/bin/env bash
# Builds the app as a normal user: production dependencies, compiled .env, warmed-up Symfony cache.
set -euo pipefail
cd "$(dirname "$0")"
export APP_ENV=prod APP_DEBUG=0
# Warming up the cache creates the Doctrine connection service, which needs the env var, but never connects.
export SQLITE_PATH=/nonexistent/feed.db

composer install --no-dev --optimize-autoloader --no-interaction --no-progress
composer dump-env prod
bin/console cache:warmup
