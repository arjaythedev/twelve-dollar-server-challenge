# Optional active-table width for either the fast or the calendar TCP build.
# The underlying stack objects and HTTP/SQLite server retain their ordinary ABI.
ifndef TWELVE_LWIP_WIDE_MK
TWELVE_LWIP_WIDE_MK := 1
include bypass/slow.mk
WIDE_BASE ?= fast
ifeq ($(filter $(WIDE_BASE),fast slow),)
$(error WIDE_BASE must be fast or slow)
endif
wide_patch = bypass/lwip-wide-active.patch
wide_key := $(word 1,$(shell sha256sum bypass/lwip-wide-active.patch))
wide_build = $(lwip_$(WIDE_BASE)_build)/wide-$(wide_key)
wide_source = $(wide_build)/source
wide_stamp = $(wide_source)/.index-ready
wide_base_flags = $(lwip_$(WIDE_BASE)_flags)
wide_base_objects = $(lwip_$(WIDE_BASE)_objects)
wide_base_tcp = $(lwip_$(WIDE_BASE)_build)/lwip/core/tcp.o
wide_tcp = $(wide_build)/tcp.o
wide_objects = $(subst $(wide_base_tcp),$(wide_tcp),$(wide_base_objects))

# The fast-wide port and the independently selectable slow-wide
# comparison can appear in one invocation, so give both TCP objects rules.
wide_fast_build = $(lwip_fast_build)/wide-$(wide_key)
wide_fast_tcp = $(wide_fast_build)/tcp.o
wide_fast_objects = $(subst $(lwip_fast_build)/lwip/core/tcp.o,$(wide_fast_tcp),$(lwip_fast_objects))
wide_slow_build = $(lwip_slow_build)/wide-$(wide_key)
wide_slow_tcp = $(wide_slow_build)/tcp.o
wide_stamps = $(addsuffix /source/.index-ready,$(wide_fast_build) $(wide_slow_build))

$(wide_stamps): %/wide-$(wide_key)/source/.index-ready: %/source/.index-ready $(wide_patch) bypass/index-lwip.sh
	bash bypass/index-lwip.sh "$(dir $<)" "$(@D)" "$(wide_patch)"

$(wide_fast_tcp): wide_compile_flags = $(lwip_fast_flags)
$(wide_slow_tcp): wide_compile_flags = $(lwip_slow_flags)
$(wide_fast_tcp) $(wide_slow_tcp): %/tcp.o: %/source/.index-ready bypass/lwipopts.h bypass/arch/cc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(wide_compile_flags) -DLWIP_TCP_ACTIVE_INDEX_BITS=20 -MMD -MP -c "$(@D)/source/src/core/tcp.c" -o "$@"

bin/server-bypass-$(WIDE_BASE)-wide: $(app_objects) $(bypass_io) $(lwip_$(WIDE_BASE)_build)/stack.o $(wide_objects) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(wide_build)/reference-test: bypass/test-wide-active.c bypass/test-pcb-index.c $(wide_base_objects) | $(wide_stamp)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(wide_base_flags) $(LDFLAGS) $< $(wide_base_objects) -o $@

$(wide_build)/candidate-test: bypass/test-wide-active.c bypass/test-pcb-index.c $(wide_objects)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(wide_base_flags) $(LDFLAGS) $< $(wide_objects) -o $@

.PHONY: check-wide-active measure-wide-active
check-wide-active: $(wide_build)/reference-test $(wide_build)/candidate-test
	$(wide_build)/reference-test > $(wide_build)/reference-lifecycle.txt
	$(wide_build)/candidate-test > $(wide_build)/candidate-lifecycle.txt
	cmp $(wide_build)/reference-lifecycle.txt $(wide_build)/candidate-lifecycle.txt
	cat $(wide_build)/candidate-lifecycle.txt
	$(wide_build)/reference-test 512 3000 32 23
	$(wide_build)/candidate-test 512 3000 32 23

measure-wide-active: check-wide-active
	@set -e; for spec in "64 0" "256 31"; do \
	    for arm in reference candidate candidate reference; do \
	        echo "$$arm"; $(wide_build)/$$arm-test 350000 145000 $$spec; \
	    done; \
	done
-include $(wide_fast_tcp:.o=.d) $(wide_slow_tcp:.o=.d)
endif
