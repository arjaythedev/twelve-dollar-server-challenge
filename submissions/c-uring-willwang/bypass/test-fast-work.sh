#!/usr/bin/env bash
# Compare the original indexed stack and the fast-timer variant in memory.
set -euo pipefail
if [[ $# != 1 && $# != 4 ]]; then
    echo "Usage: $0 UNMODIFIED_LWIP_2_2_1 [CONNECTIONS REQUESTS_PER_FAST_TICK TICKS]" >&2
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
probes: $(lwip_index_objects) $(lwip_fast_objects)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_index_flags) bypass/test-fast-work.c $(lwip_index_objects) -o $(BUILD)/indexed-probe
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) bypass/test-fast-work.c $(lwip_fast_objects) -o $(BUILD)/fast-probe
MAKE
for mode in indexed fast; do
    probe="$work/$mode-probe"
    echo "$mode"
    {
        "$probe"
        "$probe" 128 32 4
        if [[ $# == 3 ]]; then "$probe" "$@"; fi
    } | tee "$work/$mode.log"
    awk '{for (i=1; i<=NF; i++) if ($i ~ /^(edges|retry_calls|trace_packets|hash|pcbs|reqs_per_tick|ticks|reqs|packets|rcv_bytes)=/) printf "%s ", $i; print ""}' \
        "$work/$mode.log" > "$work/$mode.trace"
done
cmp "$work/indexed.trace" "$work/fast.trace"
echo "TCP traces match"
