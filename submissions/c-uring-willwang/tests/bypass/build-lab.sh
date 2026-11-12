#!/bin/sh
set -eu
if [ "$#" -ne 1 ]; then
    echo 'Usage: sh build-lab.sh WORK_DIRECTORY' >&2
    exit 2
fi
scripts=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
work=$(CDPATH='' cd -- "$1" && pwd)
sh "$scripts/../official/build-client.sh" "$work"
sh "$scripts/../official/veth/build.sh" "$work"
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -static "$scripts/packet-fd.c" -o "$work/packet-fd"
