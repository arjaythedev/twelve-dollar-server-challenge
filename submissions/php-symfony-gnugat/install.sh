#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs FrankenPHP's release binary (PHP 8.5 ZTS built in, with the
# extensions the app needs), provenance-verified, and Composer, checksum-verified.
set -euo pipefail

FRANKENPHP_VERSION=1.13.5
COSIGN_VERSION=3.1.4
case "$(uname -m)" in
  x86_64) COSIGN_ARCH=amd64; COSIGN_SHA256=424e2c97d0b45fd7a6b608a87f090f35633f4c23ece5f092d48bba11aaf4bbfa ;;
  aarch64) COSIGN_ARCH=arm64; COSIGN_SHA256=ade83b7aac62e131cacfe96d958948b2150cc9efec46a570a2649b8e27dedad2 ;;
  *) echo "unsupported architecture: $(uname -m)"; exit 1 ;;
esac
COMPOSER_VERSION=2.11.1
COMPOSER_SHA256=6f1b891e1d4927d62740276c540fb490e47689e19e102c49e8da849794882f8c

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl jq unzip

curl -fsSL -o /usr/local/bin/frankenphp \
  "https://github.com/php/frankenphp/releases/download/v${FRANKENPHP_VERSION}/frankenphp-linux-$(uname -m)-gnu"

# FrankenPHP rebuilds its latest release's binaries every night and replaces them on the release (its "Build binary
# releases" workflow), so their checksum changes from one day to the next. Instead, cosign (checksum-verified) checks
# the binary's GitHub build attestation: built by php/frankenphp's static.yaml workflow, from the release's tag or
# from a nightly rebuild of it (which runs on main).
curl -fsSL -o /tmp/cosign \
  "https://github.com/sigstore/cosign/releases/download/v${COSIGN_VERSION}/cosign-linux-${COSIGN_ARCH}"
echo "${COSIGN_SHA256}  /tmp/cosign" | sha256sum -c -
chmod +x /tmp/cosign
FRANKENPHP_SHA256="$(sha256sum /usr/local/bin/frankenphp | cut -d ' ' -f 1)"
curl -fsSL "https://api.github.com/repos/php/frankenphp/attestations/sha256:${FRANKENPHP_SHA256}" \
  | jq '.attestations[0].bundle' > /tmp/frankenphp.sigstore.json
# The binary is larger than the 128 MiB cosign hashes by default
COSIGN_MAX_ATTACHMENT_SIZE=512MiB /tmp/cosign verify-blob-attestation \
  --bundle /tmp/frankenphp.sigstore.json \
  --type https://slsa.dev/provenance/v1 \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  --certificate-identity-regexp "^https://github\.com/php/frankenphp/\.github/workflows/static\.yaml@refs/(heads/main|tags/v${FRANKENPHP_VERSION//./\\.})\$" \
  /usr/local/bin/frankenphp
rm /tmp/cosign /tmp/frankenphp.sigstore.json
chmod +x /usr/local/bin/frankenphp
# A nightly rebuild's attestation names main, not the tag it built: the binary says which release it is
FRANKENPHP_BUILT="$(/usr/local/bin/frankenphp version)"
[[ "${FRANKENPHP_BUILT}" == "FrankenPHP v${FRANKENPHP_VERSION} PHP 8.5."* ]] \
  || { echo "not FrankenPHP ${FRANKENPHP_VERSION} with PHP 8.5: ${FRANKENPHP_BUILT}"; exit 1; }

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
