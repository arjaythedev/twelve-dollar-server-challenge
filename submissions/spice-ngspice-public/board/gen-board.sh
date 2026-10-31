#!/usr/bin/env bash
# gen-board.sh <obj_dir> <chip.so> < board.cir.in > board.cir: wire the chip's pins in the order Verilator
# generated them (inputs.h / outputs.h list the ports; a bus is listed MSB first, one node per bit).
set -euo pipefail
pins() { sed -n 's/^VL_DATA([0-9]*,\([a-z0-9_]*\),\([0-9]*\),\([0-9]*\))$/\1 \2 \3/p' "$1" |
  while read -r n msb lsb; do
    if [ "$msb" = 0 ]; then printf '%s ' "$n"; else for ((i = msb; i >= lsb; i--)); do printf '%s%d ' "$n" "$i"; done; fi
  done; }
in=$(pins "$1/inputs.h"); out=$(pins "$1/outputs.h")
sed -e "s|@IN@|${in% }|" -e "s|@OUT@|${out% }|" -e "s|@SO@|$2|"
