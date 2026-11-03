#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Installs the whole runtime: the official FrankenPHP
# static binary and a pinned Composer, both run through FrankenPHP's
# embedded PHP, so no other PHP is installed on the box.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

FRANKENPHP_VERSION=1.13.3
case "$(uname -m)" in
  x86_64)  FRANKENPHP_ASSET=frankenphp-linux-x86_64-gnu ;;
  aarch64) FRANKENPHP_ASSET=frankenphp-linux-aarch64-gnu ;;
  *) echo "unsupported architecture: $(uname -m)"; exit 1 ;;
esac
COMPOSER_VERSION=2.10.3
COMPOSER_SHA256=7a2d379d5b8ffdaa028580ef26494c36d2feef4b178d3dd1473a4dbc5e17c8d6

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl git unzip

curl -fsSL -o /usr/local/bin/frankenphp \
  "https://github.com/php/frankenphp/releases/download/v${FRANKENPHP_VERSION}/${FRANKENPHP_ASSET}"
chmod 0755 /usr/local/bin/frankenphp
sha256sum /usr/local/bin/frankenphp
frankenphp version | grep -F "v${FRANKENPHP_VERSION} " || { echo "unexpected FrankenPHP build: $(frankenphp version)"; exit 1; }

mkdir -p /usr/local/lib/composer
curl -fsSL -o /usr/local/lib/composer/composer.phar \
  "https://getcomposer.org/download/${COMPOSER_VERSION}/composer.phar"
echo "${COMPOSER_SHA256}  /usr/local/lib/composer/composer.phar" | sha256sum -c -
cat > /usr/local/lib/composer/run.php <<'PHP'
<?php
// `frankenphp php-cli` puts the binary in $argv[0] and the script in $argv[1]; Composer expects
// $argv[0] to be the script. Shift once, then hand over to the pinned composer.phar.
foreach ($_SERVER['argv'] as $i => $arg) {
    if ($i > 0 && realpath($arg) === __FILE__) {
        $_SERVER['argv'] = array_slice($_SERVER['argv'], $i);
        $_SERVER['argc'] = count($_SERVER['argv']);
        $argv = $_SERVER['argv'];
        $argc = $_SERVER['argc'];
        break;
    }
    if ($i > 1) {
        break;
    }
}
require __DIR__.'/composer.phar';
PHP
cat > /usr/local/bin/composer <<'SH'
#!/usr/bin/env bash
exec /usr/local/bin/frankenphp php-cli /usr/local/lib/composer/run.php "$@"
SH
chmod 0755 /usr/local/bin/composer

frankenphp version
# install.sh runs as root, which Composer refuses unless told so...
COMPOSER_ALLOW_SUPERUSER=1 composer --no-plugins --version
