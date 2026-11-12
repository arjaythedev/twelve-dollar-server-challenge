#!/usr/bin/env bash
set -euo pipefail
usage() { echo 'Usage: install.sh [--bypass | --bypass-unit UNIT.service INTERFACE IPV4/PREFIX [GATEWAY]]'; }
mode=${1:-}
case $mode in
  ''|--bypass) (( $# < 2 )) || { usage >&2; exit 2; } ;;
  --bypass-unit)
    shift
    unit=${1:-}
    rendered=$(bash "$(dirname "$0")/bypass/systemd-drop-in.sh" "$@")
    [[ -d /run/systemd/system ]] || { echo 'The requested unit requires a running systemd system manager.' >&2; exit 1; }
    ;;
  --help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac
apt-get update
apt-get install -y --no-install-recommends build-essential clang-18 llvm-18-linker-tools binutils ca-certificates curl unzip tar gzip patch linux-libc-dev 'libssl-dev=3.0.13-*'

# Ubuntu normally builds the packet data paths into the kernel itself. On a
# custom modular build, load the standard packet-socket module if requested.
if [[ -n $mode ]]; then
  config="/boot/config-$(uname -r)"
  if [[ -r $config ]]; then
    if grep -qx 'CONFIG_PACKET=m' "$config"; then
      apt-get install -y --no-install-recommends kmod
      modprobe af_packet
    fi
    grep -E '^(CONFIG_PACKET|CONFIG_XDP_SOCKETS|CONFIG_BPF_SYSCALL)=' "$config" || true
  else
    echo 'Kernel config unavailable. The selected link will probe the running kernel at startup.'
  fi
  if [[ $mode == --bypass-unit ]]; then
    unit_dir="/etc/systemd/system/$unit.d"
    install -d -m 0755 "$unit_dir"
    tmp=$(mktemp "$unit_dir/.twelve-dollar-bypass.XXXXXXXX")
    trap 'rm -f -- "$tmp"' EXIT
    printf '%s\n' "$rendered" > "$tmp"
    chmod 0644 "$tmp"
    mv -T "$tmp" "$unit_dir/60-twelve-dollar-bypass.conf"
    systemctl daemon-reload
    echo "Installed the XDP startup override for $unit. Build with build.sh before starting the unit."
  else
    echo 'Build with build.sh, then select the interface and service address with start.sh --bypass.'
  fi
fi
