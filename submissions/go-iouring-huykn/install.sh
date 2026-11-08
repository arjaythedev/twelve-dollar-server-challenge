#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler (cgo builds SQLite), curl/unzip, and the
# official Go 1.27.3 tarball (sha256-checked) in /usr/local/go.
set -euo pipefail
apt-get update
apt-get install -y build-essential curl ca-certificates unzip
case "$(uname -m)" in
  x86_64) ARCH=amd64; SHA=65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40 ;;
  aarch64) ARCH=arm64; SHA=a06e99309dbcbdca1a8ccd2282c3ee146e55930886e26b4f1193269587165f49 ;;
  *) echo "unsupported architecture $(uname -m)"; exit 1 ;;
esac
TGZ="$(mktemp)"
curl -fsSL --retry 3 -o "$TGZ" "https://go.dev/dl/go1.27.3.linux-$ARCH.tar.gz"
echo "$SHA  $TGZ" | sha256sum -c -
rm -rf /usr/local/go
tar -C /usr/local -xzf "$TGZ"
rm -f "$TGZ"
/usr/local/go/bin/go version
