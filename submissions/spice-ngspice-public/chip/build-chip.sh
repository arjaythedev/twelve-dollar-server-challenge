#!/usr/bin/env bash
# build-chip.sh <out_dir>: Verilog -> C++ (Verilator) -> <out_dir>/chip.so, a model for ngspice's d_cosim.
# This is what ngspice's own scripts/vlnggen does, with optimisation on (vlnggen builds at -Os) and our
# own shim (shim.cpp: a rising edge on rst power-cycles the chip).
# It also writes <out_dir>/obj/{inputs,outputs,inouts}.h, the pin order the board netlist is wired in.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd); obj=$out/obj
shim=/usr/share/ngspice/scripts/src   # verilator_main.cpp and the ngspice headers
rm -rf "$obj"
verilator --Mdir "$obj" --prefix Vlng --top-module chip -O3 --x-assign fast -Wno-fatal --cc \
  "$here/top.v" "$here/chip.v" "$here/jscan.v" "$here/sha256.v"
# port list, as vlnggen writes it: VL_IN8(&clk,0,0); -> VL_DATA(8,clk,0,0)
for p in IN:inputs OUT:outputs INOUT:inouts; do
  d=${p%:*}; f=${p#*:}.h
  { echo "/* Generated code: do not edit. */"
    sed -n "s/^ *VL_${d}\([0-9]*\)(&\([A-Za-z0-9_]*\),\([0-9]*\),\([0-9]*\));.*/VL_DATA(\1,\2,\3,\4)/p" "$obj/Vlng.h"; } > "$obj/$f"
done
verilator --Mdir "$obj" --prefix Vlng --top-module chip -O3 --x-assign fast -Wno-fatal --cc --build --exe \
  -CFLAGS "-fpic -I$shim" -MAKEFLAGS OPT_FAST=-O2 -MAKEFLAGS OPT_SLOW=-O1 -MAKEFLAGS OPT_GLOBAL=-O2 \
  "$shim/verilator_main.cpp" "$here/shim.cpp" "$here/top.v" "$here/chip.v" "$here/jscan.v" "$here/sha256.v" >/dev/null
g++ --shared "$obj/shim.o" "$obj/verilated.o" "$obj/verilated_threads.o" "$obj/Vlng__ALL.a" \
  -pthread -lpthread -o "$out/chip.so"
echo "chip.so: $(grep -c VL_DATA "$obj/inputs.h") inputs, $(grep -c VL_DATA "$obj/outputs.h") outputs"
