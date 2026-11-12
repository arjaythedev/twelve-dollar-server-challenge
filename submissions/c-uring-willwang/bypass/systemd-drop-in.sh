#!/usr/bin/env bash
# Render the self-managed, single-IP XDP unit override without changing the host.
set -euo pipefail
usage() { echo 'Usage: systemd-drop-in.sh UNIT.service INTERFACE IPV4/PREFIX [GATEWAY]' >&2; exit 2; }
(( $# == 3 || $# == 4 )) || usage
unit=$1 interface=$2 address=$3 gateway=${4:-}
[[ $unit =~ ^[A-Za-z0-9][A-Za-z0-9_.@-]*\.service$ ]] || usage
[[ $interface =~ ^[A-Za-z0-9_.:-]{1,15}$ ]] || usage
ipv4() {
  local value=$1 octet
  [[ $value =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || return 1
  local -a octets
  IFS=. read -r -a octets <<< "$value"
  for octet in "${octets[@]}"; do
    (( 10#$octet <= 255 )) || return 1
    [[ $octet == 0 || $octet != 0* ]] || return 1
  done
}
[[ $address == */* && ${address#*/} =~ ^[0-9]{1,2}$ ]] || usage
(( 10#${address#*/} <= 32 )) && ipv4 "${address%/*}" || usage
[[ -z $gateway ]] || ipv4 "$gateway" || usage
entry=$(cd -P "$(dirname "$0")/.." && pwd)/start.sh
[[ $entry != *$'\n'* && $entry != *$'\r'* ]] || usage
# ExecStart uses systemd quoting, variable expansion and unit specifiers.
entry=${entry//\\/\\\\}
entry=${entry//\"/\\\"}
entry=${entry//%/%%}
entry=${entry//\$/\$\$}
cat <<'UNIT'
[Service]
AmbientCapabilities=
AmbientCapabilities=CAP_NET_BIND_SERVICE CAP_NET_RAW CAP_NET_ADMIN CAP_BPF
CapabilityBoundingSet=
CapabilityBoundingSet=CAP_NET_BIND_SERVICE CAP_NET_RAW CAP_NET_ADMIN CAP_BPF
LimitMEMLOCK=64M
ExecStart=
UNIT
printf 'ExecStart="%s" --bypass --mode xdp --shared-ip --xdp-rx-frames 8192 --packet-gso --interface %s --address %s' "$entry" "$interface" "$address"
if [[ -n $gateway ]]; then printf ' --gateway %s' "$gateway"; fi
printf '\n'
