#!/usr/bin/env bash
# Exercise both fast250 reference and calendar source through independent builds.
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
make --no-builtin-rules --no-print-directory -s -C "$submission" -f - check-build \
    LWIP_DIR="$upstream" BUILD="$work" CC="${CC:-cc}" \
    CPPFLAGS="-D_GNU_SOURCE" CFLAGS="${CFLAGS:--O2 -g -std=gnu11}" <<'MAKE'
include bypass/stack.mk
include bypass/slow.mk
.PHONY: check-build
check-build: $(BYPASS_FAST_STACK_OBJECTS) $(BYPASS_SLOW_STACK_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) bypass/test-slow-work.c $(lwip_fast_objects) -o $(BUILD)/reference-events
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_slow_flags) bypass/test-slow-work.c $(lwip_slow_objects) -o $(BUILD)/calendar-events
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) bypass/test-slow-shape.c $(lwip_fast_objects) -o $(BUILD)/reference-shape
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_slow_flags) bypass/test-slow-shape.c $(lwip_slow_objects) -o $(BUILD)/calendar-shape
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) bypass/test-keepalive.c $(BYPASS_FAST_STACK_OBJECTS) -o $(BUILD)/reference-keepalive
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_slow_flags) bypass/test-keepalive.c $(BYPASS_SLOW_STACK_OBJECTS) -o $(BUILD)/calendar-keepalive
MAKE
for variant in reference calendar; do
    echo "$variant"
    "$work/$variant-events" | tee "$work/$variant.events"
    "$work/$variant-keepalive" | tee "$work/$variant.keepalive"
    {
        "$work/$variant-shape" 128 32 4
        if [[ $# == 3 ]]; then "$work/$variant-shape" "$@"; fi
    } | tee "$work/$variant.shape"
    awk '{for (i=1; i<=NF; i++) if ($i ~ /^(pcbs|reqs_per_tick|ticks|reqs|packets|rcv_bytes|hash)=/) printf "%s ", $i; print ""}' \
        "$work/$variant.shape" > "$work/$variant.wire"
done
cmp "$work/reference.events" "$work/calendar.events"
cmp "$work/reference.keepalive" "$work/calendar.keepalive"
cmp "$work/reference.wire" "$work/calendar.wire"
echo "Timed events, TCP payloads and keepalive checks match."
