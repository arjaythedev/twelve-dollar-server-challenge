#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
apt-get update
apt-get install -y build-essential ca-certificates curl
version=1.27.3
case "$(uname -m)" in
  x86_64) arch=amd64; checksum=65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40 ;;
  aarch64|arm64) arch=arm64; checksum='' ;;
  *) echo 'Unsupported architecture' >&2; exit 1 ;;
esac
archive="go${version}.linux-${arch}.tar.gz"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
curl -fsSL --retry 3 "https://go.dev/dl/$archive" -o "$tmp_dir/$archive"
if [ -z "$checksum" ]; then
  checksum="$(curl -fsSL --retry 3 "https://go.dev/dl/$archive.sha256")"
fi
printf '%s  %s\n' "$checksum" "$tmp_dir/$archive" | sha256sum --check --status
rm -rf /usr/local/go
tar -C /usr/local -xzf "$tmp_dir/$archive"
/usr/local/go/bin/go version
