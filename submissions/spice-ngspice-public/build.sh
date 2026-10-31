#!/usr/bin/env bash
# build.sh: Verilog -> chip.so (Verilator), then the board netlist wired to its pins. ~10-60 s.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
bash "$here/chip/build-chip.sh" "$here/build"
bash "$here/board/gen-board.sh" "$here/build/obj" "$here/build/chip.so" < "$here/board/board.cir.in" > "$here/build/board.cir"
echo "built $here/build/board.cir"
