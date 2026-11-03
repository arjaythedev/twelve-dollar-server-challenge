#!/usr/bin/env bash
set -euo pipefail
# Ubuntu 24.04; run as root. Build the pinned runtime once, outside the application.
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
