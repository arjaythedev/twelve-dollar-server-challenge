#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler, curl, and the Nexium
# compiler (nx), pinned to one release and checked by SHA-256.
#
# nx comes from the project's official wheel on PyPI. A wheel is a zip file
# holding one self-contained binary, so it is unpacked directly: no pip, no
# virtualenv, and nothing else of Python's is used.
# Set NX_WHEEL to a local copy of the wheel to install offline.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential curl ca-certificates python3-minimal

NX_VER=1.5.0
NX_WHL="nexium_lang-${NX_VER}-py3-none-manylinux_2_17_x86_64.whl"
NX_URL="https://files.pythonhosted.org/packages/2e/95/b66209c19bf2088a63460ca25bf1e36e9876ca292d8e0a4d2776ad08335b/${NX_WHL}"
NX_SHA256=a5fa51c7040253aff1e2912cfd0d03fe09db61494a5ba2d49257dc834c8b6704
NX_DIR="/opt/nexium-${NX_VER}"

if [ ! -x "${NX_DIR}/bin/nx" ]; then
  tmp="$(mktemp -d)"
  whl="${NX_WHEEL:-${tmp}/${NX_WHL}}"
  [ -f "$whl" ] || curl -fsSL "$NX_URL" -o "$whl"
  echo "${NX_SHA256}  ${whl}" | sha256sum -c --quiet -
  python3 -I -m zipfile -e "$whl" "${tmp}/wheel"
  install -D -m 0755 "${tmp}/wheel/nexium_lang/bin/nx" "${NX_DIR}/bin/nx"
  install -D -m 0644 "${tmp}/wheel/nexium_lang/LICENSE" "${NX_DIR}/LICENSE"
fi
ln -sf "${NX_DIR}/bin/nx" /usr/local/bin/nx
NX_OFFLINE=1 NX_NO_UPDATE_CHECK=1 nx version
