#!/usr/bin/env bash
set -euo pipefail
# Ubuntu 24.04; run as root. Builds pinned Erlang/OTP from verified source, unpacks the pinned Elixir release.
apt-get update
apt-get install -y --no-install-recommends build-essential libssl-dev libncurses-dev curl ca-certificates unzip
prefix=/opt/erlang/29.1.1
if [[ ! -x $prefix/bin/erl ]]; then
  source_dir=$(mktemp -d)
  trap 'rm -rf "$source_dir"' EXIT
  curl -fsSL --retry 3 https://github.com/erlang/otp/releases/download/OTP-29.1.1/otp_src_29.1.1.tar.gz -o "$source_dir/otp.tar.gz"
  printf '%s  %s\n' 054e0143e39c780e091107fc9b345792a9c1a55f6bac1eca1c1101510fc06bf6 "$source_dir/otp.tar.gz" | sha256sum -c -
  tar -xzf "$source_dir/otp.tar.gz" --strip-components=1 -C "$source_dir"
  cd "$source_dir"
  ./configure --prefix="$prefix" --without-javac --without-wx --without-odbc \
    --without-debugger --without-observer --without-et
  make -j"${BUILD_JOBS:-1}"
  make install
fi
elixir=/opt/elixir/1.20.5
if [[ ! -x $elixir/bin/elixirc ]]; then
  tmp=$(mktemp -d)
  curl -fsSL --retry 3 https://github.com/elixir-lang/elixir/releases/download/v1.20.5/elixir-otp-29.zip -o "$tmp/elixir.zip"
  printf '%s  %s\n' f424d6f0bb710a1d4898f2645d8ad6cab3f5eab8d71848c0dda2579d3a8e8a06 "$tmp/elixir.zip" | sha256sum -c -
  mkdir -p "$elixir" && unzip -q "$tmp/elixir.zip" -d "$elixir"
  chmod +x "$elixir"/bin/*
  rm -rf "$tmp"
fi
