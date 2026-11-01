#!/usr/bin/env bash
# build.sh: Verilog -> chip.so (Verilator), nic.so (the network card / storage controller), then the board
# netlist wired to the chip's pins. ~10-60 s.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); b=$here/build
bash "$here/chip/build-chip.sh" "$b"
gcc -O2 -Wall -shared -fPIC -Wl,-z,nodelete -I/usr/share/ngspice/scripts/src "$here/nic/nic.c" -lsqlite3 -o "$b/nic.so"
bash "$here/board/gen-board.sh" "$b/obj" "$b/chip.so" "$b/nic.so" < "$here/board/board.cir.in" > "$b/board.cir"
echo "built $b/board.cir"
