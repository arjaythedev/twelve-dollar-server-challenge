#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs FrankenPHP's release binary (PHP 8.5 ZTS built in, with the
# extensions the app needs) and Composer, both checksum-verified.
set -euo pipefail

FRANKENPHP_VERSION=1.13.4
case "$(uname -m)" in
  x86_64) FRANKENPHP_SHA256=d4d032529a2553bf91034a47ded3f1f1b99c1630541008b3ce1ce0d28f01dbcc ;;
  aarch64) FRANKENPHP_SHA256=439922840c5afaba450ce9a7c1863dd740bbd4d53591d0f6bff5fd1945b2c0a7 ;;
  *) echo "unsupported architecture: $(uname -m)"; exit 1 ;;
esac
COMPOSER_VERSION=2.11.1
COMPOSER_SHA256=6f1b891e1d4927d62740276c540fb490e47689e19e102c49e8da849794882f8c

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl unzip

curl -fsSL -o /usr/local/bin/frankenphp \
  "https://github.com/php/frankenphp/releases/download/v${FRANKENPHP_VERSION}/frankenphp-linux-$(uname -m)-gnu"
echo "${FRANKENPHP_SHA256}  /usr/local/bin/frankenphp" | sha256sum -c -
chmod +x /usr/local/bin/frankenphp

# PHP's command line, for build.sh: `frankenphp php-cli script.php` gives the script its own path as its first argument
# (Composer and bin/console would read it as their command), so the shim drops everything up to itself, then runs the
# script. Composer runs its scripts with PHP_BINARY, which would be frankenphp itself.
mkdir -p /usr/local/lib/frankenphp-php
cat > /usr/local/lib/frankenphp-php/argv.php <<'PHP'
<?php
$argv = array_slice($_SERVER['argv'], array_search(__FILE__, $_SERVER['argv'], true) + 1);
$argc = count($argv);
$_SERVER['argv'] = $argv;
$_SERVER['argc'] = $argc;
$_SERVER['SCRIPT_NAME'] = $_SERVER['SCRIPT_FILENAME'] = $_SERVER['PHP_SELF'] = $argv[0];
require $argv[0];
PHP
cat > /usr/local/bin/frankenphp-php <<'SH'
#!/usr/bin/env bash
export PHP_BINARY=/usr/local/bin/frankenphp-php
exec /usr/local/bin/frankenphp php-cli /usr/local/lib/frankenphp-php/argv.php "$@"
SH
chmod +x /usr/local/bin/frankenphp-php

curl -fsSL -o /usr/local/bin/composer "https://getcomposer.org/download/${COMPOSER_VERSION}/composer.phar"
echo "${COMPOSER_SHA256}  /usr/local/bin/composer" | sha256sum -c -
chmod +x /usr/local/bin/composer
