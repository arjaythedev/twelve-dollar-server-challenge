#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04. Installs a C toolchain (cgo compiles the SQLite
# amalgamation) and Go into /usr/local/go.
#
# Architecture-aware on purpose: the benchmark box is x86-64 and the local Docker rig is arm64.
# Both tarball checksums are pinned, so the amd64 path is exercised by CI and the box, the arm64
# path by the rig.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates curl unzip openssl

GO_VERSION=1.27.3
case "$(dpkg --print-architecture)" in
  amd64) GO_ARCH=amd64; GO_SHA256=65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40 ;;
  arm64) GO_ARCH=arm64; GO_SHA256=a06e99309dbcbdca1a8ccd2282c3ee146e55930886e26b4f1193269587165f49 ;;
  *) echo "unsupported architecture: $(dpkg --print-architecture)" >&2; exit 1 ;;
esac

tarball="$(mktemp)"
trap 'rm -f "$tarball"' EXIT
curl -fsSL --retry 3 -o "$tarball" "https://go.dev/dl/go${GO_VERSION}.linux-${GO_ARCH}.tar.gz"
printf '%s  %s\n' "$GO_SHA256" "$tarball" | sha256sum -c -

rm -rf /usr/local/go
tar -C /usr/local -xzf "$tarball"
/usr/local/go/bin/go version
