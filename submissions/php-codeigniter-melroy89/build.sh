#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
composer install --no-dev --classmap-authoritative --no-interaction --prefer-dist
# Worker mode keeps metadata in-process. CI4 explicitly forbids spark optimize here.
find app public -name '*.php' -print0 | xargs -0 -n1 php -l >/dev/null
