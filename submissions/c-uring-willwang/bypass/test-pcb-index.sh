#!/usr/bin/env bash
# Compile and run the same in-memory TCP regression against both stack variants.
# With CONNECTIONS and PACKETS, also measure shuffled real TCP PCB lookups and timers.
set -euo pipefail
if [[ $# != 1 && $# != 3 ]]; then
    echo "Usage: $0 UNMODIFIED_LWIP_2_2_1 [CONNECTIONS PACKETS]" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
submission=$(cd "$here/.." && pwd)
upstream=$(cd "$1" && pwd)
shift
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
make --no-builtin-rules --no-print-directory -s -C "$submission" -f - probes \
    LWIP_DIR="$upstream" BUILD="$work" CC="${CC:-cc}" \
    CPPFLAGS="-D_GNU_SOURCE" CFLAGS="${CFLAGS:--O2 -g -std=gnu11}" <<'MAKE'
include bypass/stack.mk
.PHONY: probes
probes: $(lwip_objects) $(lwip_index_objects)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_flags) bypass/test-pcb-index.c $(lwip_objects) -o $(BUILD)/stock-probe
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_index_flags) bypass/test-pcb-index.c $(lwip_index_objects) -o $(BUILD)/indexed-probe
MAKE
for mode in stock indexed; do
    echo "$mode"
    "$work/$mode-probe"
    if [[ $# == 2 ]]; then "$work/$mode-probe" "$1" "$2" 1; fi
done
