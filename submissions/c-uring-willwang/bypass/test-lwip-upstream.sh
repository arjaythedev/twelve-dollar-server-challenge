#!/usr/bin/env bash
# Run unmodified upstream TCP, TCP_OOS and TCP_STATE cases using their Check runner.
set -euo pipefail
if [[ $# != 1 ]]; then
    echo "Usage: $0 FULL_UNMODIFIED_LWIP_2_2_1_RELEASE_TREE" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
submission=$(cd "$here/.." && pwd)
upstream=$(cd "$1" && pwd)
unit="$upstream/test/unit"
if [[ ! -f "$unit/lwip_unittests.c" ]]; then
    echo "Use the complete lwIP release containing test/unit/lwip_unittests.c." >&2
    exit 2
fi
cc=${CC:-cc}
if [[ -z ${CHECK_CFLAGS+x} || -z ${CHECK_LIBS+x} ]]; then
    if ! pkg-config --exists check; then
        echo "Install the Check development library or supply CHECK_CFLAGS and CHECK_LIBS." >&2
        exit 2
    fi
fi
read -r -a check_cflags <<< "${CHECK_CFLAGS-$(pkg-config --cflags check)}"
read -r -a check_libs <<< "${CHECK_LIBS-$(pkg-config --libs check)}"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
make --no-builtin-rules --no-print-directory -s -C "$submission" -f - trees BUILD="$work" LWIP_DIR="$upstream" <<'MAKE'
include bypass/stack.mk
include bypass/ack-output.mk
.PHONY: trees
trees: $(lwip_fast_stamp) $(lwip_slow_stamp) $(ack_output_source)
	@printf '%s\n' "$(LWIP_DIR)" "$(lwip_fast_dir)" "$(lwip_slow_dir)" "$(lwip_fast_dir)" > "$(BUILD)/trees"
	@printf '%s\n' "$(ack_output_source)" > "$(BUILD)/retained-tcp"
	@printf '%s\n' $(lwip_units) > "$(BUILD)/units"
MAKE
printf '#define HAVE_STDINT_H 1\n' > "$work/config.h"
# Retain upstream assertions, leak checks, fixtures and test functions; select
# only the three suites supported by this IPv4 NO_SYS port.
python3 - "$unit/lwip_unittests.c" "$work/runner.c" <<'PY'
import sys
from pathlib import Path
source = Path(sys.argv[1]).read_text()
anchor = "  suite_getter_fn* suites[] = {"
before, found, tail = source.partition(anchor)
suites, end, after = tail.partition("\n  };")
names = ("tcp_suite", "tcp_oos_suite", "tcp_state_suite")
if not found or not end or not all(name in suites for name in names):
    raise SystemExit("Unexpected lwIP unit runner")
port = """#include "lwip/stats.h"
#include "lwip/memp.h"
#include "lwip/sys.h"
#include <time.h>
u32_t sys_now(void) { return (u32_t)time(NULL) * 1000U; }
"""
Path(sys.argv[2]).write_text(port + before + anchor + "\n    " + ", ".join(names) + end + after)
PY
mapfile -t trees < "$work/trees"
mapfile -t units < "$work/units"
names=(stock fast calendar retained-ack)
retained_tcp=$(<"$work/retained-tcp")
for i in "${!names[@]}"; do
    tree=${trees[$i]}
    name=${names[$i]}
    obj="$work/unit-$name"
    mkdir -p "$obj"
    indexes=0; fast=0; slow=0
    if ((i >= 1)); then indexes=17; fast=1; fi
    if ((i == 2)); then slow=1; fi
    flags=(-O1 -std=gnu11 -D_GNU_SOURCE -DLWIP_UNITTESTS_FORK
           -DLWIP_TCP_TUPLE_INDEX_BITS="$indexes" -DLWIP_TCP_FAST_WORK="$fast" -DLWIP_TCP_SLOW_WORK="$slow"
           -I"$here/tests/upstream" -I"$here" -I"$work" -I"$tree/src/include" -I"$unit" "${check_cflags[@]}")
    if [[ $name == retained-ack ]]; then flags+=(-iquote "$tree/src/core"); fi
    for u in "${units[@]}"; do
        input="$tree/src/$u.c"
        if [[ $name == retained-ack && $u == core/tcp ]]; then input=$retained_tcp; fi
        "$cc" "${flags[@]}" -c "$input" -o "$obj/${u//\//_}.o"
    done
    for u in tcp_helper test_tcp test_tcp_oos test_tcp_state; do
        "$cc" "${flags[@]}" -c "$unit/tcp/$u.c" -o "$obj/$u.o"
    done
    "$cc" "${flags[@]}" "$work/runner.c" "$obj"/*.o "${check_libs[@]}" -o "$obj/run"
    echo "$name"
    (cd "$obj" && ./run)
done
