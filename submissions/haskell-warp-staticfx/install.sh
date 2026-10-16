#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: pinned GHC 9.10.3 and cabal-install 3.16.1.0 via ghcup 0.2.6.2,
# installed to /opt/ghcup and linked into /usr/local/bin. (Ubuntu's cabal-install 3.8 can't verify Hackage's
# current root keys, so `cabal update` fails with it.)
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates xz-utils \
  libgmp-dev libffi-dev libncurses-dev zlib1g-dev
curl -fsSL -o /usr/local/bin/ghcup "https://downloads.haskell.org/~ghcup/0.2.6.2/$(uname -m)-linux-ghcup-0.2.6.2"
chmod +x /usr/local/bin/ghcup
export GHCUP_INSTALL_BASE_PREFIX=/opt
ghcup install ghc 9.10.3 --set
ghcup install cabal 3.16.1.0 --set
chmod -R a+rX /opt/.ghcup
ln -sf /opt/.ghcup/bin/ghc /opt/.ghcup/bin/ghc-pkg /opt/.ghcup/bin/cabal /usr/local/bin/
ghc --version && cabal --version
