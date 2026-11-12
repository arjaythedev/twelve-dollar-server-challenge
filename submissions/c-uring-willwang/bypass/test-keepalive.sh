#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 1 ]]; then
    echo "Usage: $0 UNMODIFIED_LWIP_2_2_1" >&2
    exit 2
fi
submission=$(cd "$(dirname "$0")/.." && pwd)
upstream=$(cd "$1" && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
make --no-builtin-rules --no-print-directory -s -C "$submission" -f - check \
    LWIP_DIR="$upstream" BUILD="$work" CC="${CC:-cc}" \
    CPPFLAGS="-D_GNU_SOURCE" CFLAGS="${CFLAGS:--O2 -std=gnu11}" <<'MAKE'
include bypass/stack.mk
.PHONY: check
check: $(BYPASS_STACK_OBJECTS) $(BYPASS_INDEXED_STACK_OBJECTS) $(BYPASS_FAST_STACK_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_flags) bypass/test-keepalive.c $(BYPASS_STACK_OBJECTS) -o $(BUILD)/stock-test
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_index_flags) bypass/test-keepalive.c $(BYPASS_INDEXED_STACK_OBJECTS) -o $(BUILD)/indexed-test
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) bypass/test-keepalive.c $(BYPASS_FAST_STACK_OBJECTS) -o $(BUILD)/fast-test
	$(BUILD)/stock-test
	$(BUILD)/indexed-test
	$(BUILD)/fast-test
MAKE
