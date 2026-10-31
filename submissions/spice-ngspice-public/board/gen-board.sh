#!/usr/bin/env bash
# gen-board.sh <obj_dir> <chip.so> <nic.so> < board.cir.in > board.cir: wire the chip's pins in the order Verilator
# generated them (inputs.h / outputs.h list the ports; a bus is listed MSB first, one node per bit).
set -euo pipefail
pins() { sed -n 's/^VL_DATA([0-9]*,\([a-z0-9_]*\),\([0-9]*\),\([0-9]*\))$/\1 \2 \3/p' "$1" |
  while read -r n msb lsb; do
    [ "$n" = cclk ] && continue   # the core clock: shim.cpp drives it
    if [ "$msb" = 0 ]; then printf '%s ' "$n"; else for ((i = msb; i >= lsb; i--)); do printf '%s%d ' "$n" "$i"; done; fi
  done; }
in=$(pins "$1/inputs.h"); out=$(pins "$1/outputs.h")
# @bus[msb:lsb]@ in the netlist: the bus's nodes, MSB first
bus() { local l=$1; while [[ $l =~ @([a-z]+)\[([0-9]+):([0-9]+)\]@ ]]; do
    local b="" i; for ((i = BASH_REMATCH[2]; i >= BASH_REMATCH[3]; i--)); do b+="${BASH_REMATCH[1]}$i "; done
    l=${l/"${BASH_REMATCH[0]}"/${b% }}; done; printf '%s\n' "$l"; }
while IFS= read -r line; do bus "$line"; done |
sed -e "s|@IN@|${in% }|" -e "s|@OUT@|${out% }|" -e "s|@SO@|$2|" -e "s|@NIC@|$3|"
