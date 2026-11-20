#!/usr/bin/env bash
# Builds bin/server as a normal user on Ubuntu 24.04 (noble, x86_64). Nothing is fetched from the network.
#
# Pins (install.sh installs all of them; this script refuses to build against anything else):
#   GO_VERSION   the Go toolchain, checked with `go env GOVERSION`
#   SQLite       Ubuntu's libsqlite3-dev / libsqlite3-0 3.45.1-1ubuntu2.8. The header version and the runtime
#                package version are both checked here.
#   Go modules   none from the network: GOPROXY=off and -mod=readonly. The only Go dependency is the in-tree
#                go-sqlite3 fork in ./sqlite3.
set -euo pipefail
cd "$(dirname "$0")"

GO_VERSION=go1.27.2
SQLITE_VERSION=3.45.1
SQLITE_PKG=3.45.1-1ubuntu2.8
# Where sqlite3.h and the libsqlite3 linker entry live. The defaults are the Ubuntu paths. Both are
# overridable, for building against an unpacked copy of the same package.
SQLITE_INCLUDE="${SQLITE_INCLUDE:-/usr/include}"
SQLITE_LIBDIR="${SQLITE_LIBDIR:-}"
GO="${GO:-/usr/local/go/bin/go}"

die() { echo "build.sh: $*" >&2; exit 1; }

[ -x "$GO" ] || die "Go not found at $GO (run install.sh, or set GO=/path/to/go/bin/go)"
[ "$("$GO" env GOVERSION)" = "$GO_VERSION" ] || die "need $GO_VERSION, found $("$GO" env GOVERSION)"
grep -q "^#define SQLITE_VERSION *\"$SQLITE_VERSION\"" "$SQLITE_INCLUDE/sqlite3.h" \
  || die "$SQLITE_INCLUDE/sqlite3.h is not SQLite $SQLITE_VERSION (run install.sh)"
RUNTIME="$(dpkg-query -W -f='${Version}\n' libsqlite3-0 2>/dev/null | sed -n 1p)"
[ "$RUNTIME" = "$SQLITE_PKG" ] || die "libsqlite3-0 is $RUNTIME, need $SQLITE_PKG (run install.sh)"

export CGO_ENABLED=1 GOTOOLCHAIN=local GOPROXY=off GOFLAGS="-mod=readonly -buildvcs=false"
# CGO_CFLAGS replaces Go's default (-g -O2), so restate it. Without -O2 the driver's cgo glue compiles unoptimized.
export CGO_CFLAGS="-g -O2 -I$SQLITE_INCLUDE"
export CGO_LDFLAGS="${SQLITE_LIBDIR:+-L$SQLITE_LIBDIR }"
mkdir -p bin
"$GO" build -tags libsqlite3 -trimpath -ldflags='-s -w' -o bin/server .

{
  echo "go:      $("$GO" env GOVERSION) (pinned $GO_VERSION)"
  echo "sqlite:  $SQLITE_VERSION ($SQLITE_PKG, Ubuntu libsqlite3-dev/libsqlite3-0)"
  echo "cflags:  $CGO_CFLAGS"
} > bin/build-info.txt
echo "build: wrote bin/server"
