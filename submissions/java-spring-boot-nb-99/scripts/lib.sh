#!/usr/bin/env bash
# Shared helpers for the mode scripts. Callers define $work, a temporary directory.
# shellcheck disable=SC2154

platform="$(uname -s)/$(uname -m)"

need() {
  command -v "$1" >/dev/null || { echo "$1 is required" >&2; exit 1; }
}

# fetch <url> <sha256> <file>
fetch() {
  curl -fsSL --retry 3 --connect-timeout 30 -o "$3" "$1"
  local actual
  if command -v sha256sum >/dev/null; then actual=$(sha256sum "$3"); else actual=$(shasum -a 256 "$3"); fi
  [ "${actual%% *}" = "$2" ] || { echo "Checksum mismatch for $1" >&2; exit 1; }
}

free_port() {
  python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

# wait_healthy <url> <pid>: waits until <url>/health answers while <pid> runs.
wait_healthy() {
  local deadline=$((SECONDS + ${WAIT_TIMEOUT:-120}))
  until curl -fsS --noproxy '*' --max-time 2 "$1/health" >/dev/null 2>&1; do
    if ! kill -0 "$2" 2>/dev/null || [ "$SECONDS" -ge "$deadline" ]; then return 1; fi
    sleep 1
  done
}

# Sets K6_BIN to an installed k6 v2, or downloads k6 2.2.0 into $work.
ensure_k6() {
  K6_BIN=${K6_BIN:-k6}
  if command -v "$K6_BIN" >/dev/null && "$K6_BIN" version 2>/dev/null | grep -q '^k6 v2\.'; then return; fi
  local name sha
  case "$platform" in
    Darwin/arm64) name=macos-arm64.zip sha=37a028506bf13578de66c906296803775df28cc2504b1a8c5d786b2803e757c7 ;;
    Darwin/x86_64) name=macos-amd64.zip sha=45a08590511c8a6a9c6331645e60c41624924e848e29472e87c60ac956d50372 ;;
    Linux/x86_64) name=linux-amd64.tar.gz sha=b5a8003c86f35f5cd5ceef1490312c48e587696c94d998cefc6d7b3b4cb1597d ;;
    Linux/aarch64 | Linux/arm64) name=linux-arm64.tar.gz sha=4ecd64cadcc792402d16293836115480419c4447c032858f564852d98f1bf54c ;;
    *) echo "No k6 download for $platform" >&2; exit 1 ;;
  esac
  echo "k6 v2 not found; downloading k6 2.2.0 to $work"
  fetch "https://github.com/grafana/k6/releases/download/v2.2.0/k6-v2.2.0-$name" "$sha" "$work/k6-archive"
  mkdir "$work/k6"
  case "$name" in
    *.zip) unzip -q "$work/k6-archive" -d "$work/k6" ;;
    *) tar -xzf "$work/k6-archive" -C "$work/k6" ;;
  esac
  K6_BIN=$(echo "$work"/k6/*/k6)
}

# Raises the open-file limit for k6 and the app as far as the OS allows; sets $files.
raise_file_limit() {
  files=$(ulimit -Hn)
  if [ "$files" = unlimited ] || [ "$files" -gt 65535 ]; then files=65535; fi
  if [ "$(uname -s)" = Darwin ]; then
    local maximum
    maximum=$(sysctl -n kern.maxfilesperproc)
    if [ "$maximum" -lt "$files" ]; then files=$maximum; fi
  fi
  ulimit -n "$files"
}
