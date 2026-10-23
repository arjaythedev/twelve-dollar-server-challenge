#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04 box.
# Installs the C toolchain, liburing, Go 1.26.3, static libjwt 3.6.1,
# and the Trux compiler commit this server was tested with.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
export GOTOOLCHAIN=local

apt-get update
apt-get install -y --no-install-recommends \
  build-essential \
  ca-certificates \
  cmake \
  curl \
  git \
  libjansson-dev \
  libssl-dev \
  liburing-dev \
  pkg-config

arch="$(uname -m)"
case "$arch" in
  x86_64) goarch=amd64; gosha=2b2cfc7148493da5e73981bffbf3353af381d5f93e789c82c79aff64962eb556 ;;
  aarch64|arm64) goarch=arm64; gosha=9d89a3ea57d141c2b22d70083f2c8459ba3890f2d9e818e7e933b75614936565 ;;
  *) echo "unsupported architecture: $arch" >&2; exit 1 ;;
esac

if ! /usr/local/go/bin/go version 2>/dev/null | grep -q 'go1.26.3 '; then
  tmp="$(mktemp)"
  curl -fsSL "https://dl.google.com/go/go1.26.3.linux-${goarch}.tar.gz" -o "$tmp"
  echo "${gosha}  ${tmp}" | sha256sum -c -
  rm -rf /usr/local/go
  tar -C /usr/local -xzf "$tmp"
  rm -f "$tmp"
fi
ln -sfn /usr/local/go/bin/go /usr/local/bin/go

libjwt_version=3.6.1
libjwt_sha=b483a5f77e548964553f54a0ec5f0c810cc6c0629c5ac5a03610bcced150e7be
libjwt_prefix=/opt/libjwt
if [[ ! -f ${libjwt_prefix}/lib/libjwt.a || "$(cat "${libjwt_prefix}/.pin" 2>/dev/null || true)" != "$libjwt_version" ]]; then
  tmp="$(mktemp)"
  curl -fsSL "https://github.com/benmcollins/libjwt/archive/refs/tags/v${libjwt_version}.tar.gz" -o "$tmp"
  echo "${libjwt_sha}  ${tmp}" | sha256sum -c -
  rm -rf /tmp/libjwt-${libjwt_version} /tmp/libjwt-build
  tar -xzf "$tmp" -C /tmp
  cmake -S "/tmp/libjwt-${libjwt_version}" -B /tmp/libjwt-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$libjwt_prefix" \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_PIC=ON
  cmake --build /tmp/libjwt-build -j"$(nproc)"
  cmake --install /tmp/libjwt-build
  echo "$libjwt_version" > "${libjwt_prefix}/.pin"
  rm -rf "/tmp/libjwt-${libjwt_version}" /tmp/libjwt-build "$tmp"
fi

# Feed responses and the 23-byte challenge secret need this compiler.
trux_commit=aefc5e2a1f0ebb24fa7476ba703377b3cf9e8357
stamp=/usr/local/lib/trux-compiler-commit
if [[ ! -x /usr/local/bin/trux || "$(cat "$stamp" 2>/dev/null || true)" != "$trux_commit" ]]; then
  src="$(mktemp -d)"
  git -C "$src" init
  git -C "$src" remote add origin https://github.com/claudioscheer/trux.git
  git -C "$src" fetch --depth 1 origin "$trux_commit"
  git -C "$src" checkout --detach FETCH_HEAD
  test "$(git -C "$src" rev-parse HEAD)" = "$trux_commit"
  (
    cd "$src"
    go build -o /usr/local/bin/trux ./cmd/trux
  )
  mkdir -p /usr/local/lib
  echo "$trux_commit" > "$stamp"
  rm -rf "$src"
fi
