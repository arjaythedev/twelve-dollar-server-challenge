#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
export XMAKE_ROOT=y
apt-get update
apt-get install -y --no-install-recommends \
  build-essential liburing-dev ca-certificates curl unzip wget git

if ! command -v xmake >/dev/null 2>&1; then
  apt-get install -y --no-install-recommends software-properties-common gnupg
  if add-apt-repository -y ppa:xmake-io/xmake && apt-get update && \
     apt-get install -y --no-install-recommends xmake; then
    :
  else
    curl -fsSL https://xmake.io/shget.text | bash
    ln -sf "$HOME/.local/bin/xmake" /usr/local/bin/xmake
  fi
fi

gcc --version | head -1
g++ --version | head -1
xmake --version | head -1
dpkg -s liburing-dev | grep '^Version'
