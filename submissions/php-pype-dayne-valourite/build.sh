#!/usr/bin/env bash
# Installs the locked dependencies (pype/framework v0.12.0 and its PSR packages, see composer.lock)
# with an authoritative classmap, and caches the routes (`bin/pype optimize`).
set -euo pipefail
cd "$(dirname "$0")"
[ -f .env ] || cp .env.example .env
composer install --no-dev --classmap-authoritative --no-interaction --no-progress
php bin/pype optimize
