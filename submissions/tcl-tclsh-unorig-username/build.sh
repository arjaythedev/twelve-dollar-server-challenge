#!/usr/bin/env bash
# Compiles tcllib 1.21's sha256c accelerator (its C SHA-256, shipped as source in the tcllib
# package) with critcl into ./lib, then checks that server.tcl at least parses.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf lib .critcl
critcl -cache "$PWD/.critcl" -libdir "$PWD/lib" -pkg sha256c /usr/share/tcltk/tcllib1.21/sha1/sha256c.tcl
echo 'if {![info complete [read [open server.tcl]]]} {exit 1}' | tclsh8.6
