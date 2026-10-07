#!/usr/bin/env bash
# Nothing to compile: PHP and Swoole come from install.sh. Lint the server so syntax errors fail the build.
set -euo pipefail
cd "$(dirname "$0")"
php -d extension=swoole -l server.php
