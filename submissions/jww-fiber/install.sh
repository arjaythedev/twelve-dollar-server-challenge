#!/usr/bin/env bash
# Installs the Go toolchain and the C compiler needed by mattn/go-sqlite3.
# Runs once as root on a clean Ubuntu 24.04.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl

GO_VERSION=1.27.3
case $(uname -m) in
  x86_64)  arch=amd64; sha=65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40 ;;
  aarch64) arch=arm64; sha=a06e99309dbcbdca1a8ccd2282c3ee146e55930886e26b4f1193269587165f49 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

archive=$(mktemp)
trap 'rm -f "$archive"' EXIT
curl -fsSL --retry 3 "https://go.dev/dl/go${GO_VERSION}.linux-${arch}.tar.gz" -o "$archive"
printf '%s  %s\n' "$sha" "$archive" | sha256sum -c -

rm -rf /usr/local/go
tar -xzf "$archive" -C /usr/local
ln -sf /usr/local/go/bin/go /usr/local/bin/go
/usr/local/go/bin/go version
