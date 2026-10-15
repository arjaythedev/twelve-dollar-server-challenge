#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04 (noble) x86_64 machine. Installs the pinned SQLite package and the
# pinned Go toolchain. Every download is checked against a version or sha256 pinned in this file.
set -euo pipefail

GO_VERSION=go1.27.2
GO_SHA256=ecbadb99091a3f46e31f5f934b068b1864eafa7995211b39eaddf76996045fe5
GO_URL="https://go.dev/dl/${GO_VERSION}.linux-amd64.tar.gz"
SQLITE_PKG=3.45.1-1ubuntu2.8   # Ubuntu noble-updates; exact version, so a point release can't change the engine under us

die() { echo "install.sh: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root: sudo bash install.sh"
# shellcheck disable=SC1091
. /etc/os-release
[ "${UBUNTU_CODENAME:-}" = noble ] || die "expects Ubuntu 24.04 (noble), found ${PRETTY_NAME:-unknown}"
[ "$(dpkg --print-architecture)" = amd64 ] || die "expects an amd64 machine"

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential ca-certificates curl unzip "libsqlite3-dev=${SQLITE_PKG}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl --fail --location --silent --show-error -o "$tmp/go.tar.gz" "$GO_URL"
echo "$GO_SHA256  $tmp/go.tar.gz" | sha256sum --check --status || die "Go tarball checksum mismatch"
rm -rf /usr/local/go
tar -C /usr/local -xzf "$tmp/go.tar.gz"

[ "$(/usr/local/go/bin/go env GOVERSION)" = "$GO_VERSION" ] || die "installed Go is not $GO_VERSION"
/usr/local/go/bin/go version
gcc --version | head -n 1
sqlite3_version="$(dpkg-query -W -f='${Version}\n' libsqlite3-0 | sed -n 1p)"
[ "$sqlite3_version" = "$SQLITE_PKG" ] || die "libsqlite3-0 is $sqlite3_version, expected $SQLITE_PKG"
echo "libsqlite3-0 $sqlite3_version"
