ifndef TWELVE_LWIP_ACK_OUTPUT_MK
TWELVE_LWIP_ACK_OUTPUT_MK := 1
include bypass/ack-deadline.mk
ack_output_dir = $(ack_build)/retain
ack_output_source = $(ack_output_dir)/tcp.c
ack_output_tcp = $(ack_output_dir)/tcp.o
ack_output_objects = $(subst $(lwip_fast_build)/lwip/core/tcp.o,$(ack_output_tcp),$(lwip_fast_objects))
BYPASS_RETAIN_ACK_STACK_OBJECTS = $(ack_port) $(ack_output_objects)

$(ack_output_source): $(lwip_fast_stamp) bypass/lwip-ack-output.patch
	@mkdir -p "$(@D)"
	@set -eu; staged=$$(mktemp "$(@D)/tcp.XXXXXXXX"); \
	    trap 'rm -f "$$staged"' EXIT; \
	    patch --batch --fuzz=0 --silent --reject-file=- --output="$$staged" "$(lwip_fast_dir)/src/core/tcp.c" < bypass/lwip-ack-output.patch; \
	    mv -f "$$staged" "$@"

$(ack_output_tcp): $(ack_output_source) $(lwip_fast_stamp) bypass/lwipopts.h bypass/arch/cc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) -fmacro-prefix-map="$<=$(lwip_fast_dir)/src/core/tcp.c" -iquote "$(lwip_fast_dir)/src/core" -MMD -MP -c "$<" -o "$@"

bin/server-bypass-fast-ack-retry: $(app_objects) $(bypass_io) $(BYPASS_RETAIN_ACK_STACK_OBJECTS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(ack_output_dir)/test-fixed-wire: bypass/test-ack-wire.c $(BYPASS_RETAIN_ACK_STACK_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) $(LDFLAGS) -Wl,--wrap=clock_gettime $^ -o $@

$(ack_output_dir)/test-original-wire: bypass/test-ack-wire.c $(BYPASS_SHORT_ACK_NARROW_STACK_OBJECTS) | $(ack_output_source)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) $(LDFLAGS) -Wl,--wrap=clock_gettime $^ -o $@

.PHONY: check-retain-ack
check-retain-ack: $(ack_output_dir)/test-original-wire $(ack_output_dir)/test-fixed-wire
	$(ack_output_dir)/test-original-wire lost
	$(ack_output_dir)/test-fixed-wire retained

-include $(ack_output_tcp:.o=.d)
endif
