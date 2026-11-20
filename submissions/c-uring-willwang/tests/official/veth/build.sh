#!/bin/sh
set -eu
[ "$#" -eq 1 ] || { echo 'Usage: build.sh WORK_DIRECTORY' >&2; exit 2; }
scripts=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1/veth-bin"
work=$(CDPATH='' cd -- "$1" && pwd)
cc_cmd=${CC:-cc}
for tool in setup show placement endpoint hold; do
    output=$tool
    [ "$tool" != placement ] || output=probe
    "$cc_cmd" -std=c11 -O2 -Wall -Wextra -Werror -static "$scripts/$tool.c" -o "$work/veth-bin/$output"
done
"$cc_cmd" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared "$scripts/allocate.c" -ldl -o "$work/veth-bin/allocate.so"
