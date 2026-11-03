#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends php8.3-cli php8.3-sqlite3 php8.3-mbstring php8.3-intl php8.3-opcache curl ca-certificates unzip

# Stage on the destination filesystem so each verified executable is replaced atomically.
# Never learn a new expected checksum from the same download being verified.
install -d /usr/local/bin
stage="$(mktemp -d /usr/local/bin/.ci4-install.XXXXXX)"
trap 'rm -rf -- "$stage"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

download_verified() {
    local name="$1" url="$2" expected="$3" actual
    curl --fail --location --silent --show-error --proto '=https' --proto-redir '=https' \
        --retry 3 --connect-timeout 15 --max-time 300 "$url" -o "$stage/$name"
    actual="$(sha256sum "$stage/$name")"
    actual="${actual%% *}"
    if [[ "$actual" != "$expected" ]]; then
        printf '%s\n' "Checksum mismatch for $name ($url)" \
            "Expected: $expected" "Actual:   $actual" \
            'Installation aborted; installed executables were not replaced.' >&2
        return 1
    fi
}

COMPOSER_VERSION=2.8.12
COMPOSER_SHA256=f446ea719708bb85fcbf4ef18def5d0515f1f9b4d703f6d820c9c1656e10a2f2
FRANKENPHP_VERSION=1.13.0
FRANKENPHP_SHA256=89e6410f56baf4b960f6429a778081524d266f9b9c8a9a14d1ba697c21e70104

download_verified composer "https://getcomposer.org/download/$COMPOSER_VERSION/composer.phar" "$COMPOSER_SHA256"
download_verified frankenphp "https://github.com/php/frankenphp/releases/download/v${FRANKENPHP_VERSION}/frankenphp-linux-x86_64-gnu" "$FRANKENPHP_SHA256"

# Verify both downloads before replacing either installed executable.
chmod 0755 "$stage/composer" "$stage/frankenphp"
mv -f -- "$stage/composer" /usr/local/bin/composer
mv -f -- "$stage/frankenphp" /usr/local/bin/frankenphp
