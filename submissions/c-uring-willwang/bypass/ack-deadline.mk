# Optional short ACK deadlines, with the same fast timer and full flow table
# as server-bypass-fast-wide. The default server's object list is unchanged.
ifndef TWELVE_LWIP_ACK_DEADLINE_MK
TWELVE_LWIP_ACK_DEADLINE_MK := 1
include bypass/wide.mk
ack_build = $(wide_fast_build)/ack-deadline
ack_source = $(ack_build)/stack.c
ack_port = $(ack_build)/stack.o
BYPASS_SHORT_ACK_STACK_OBJECTS = $(ack_port) $(wide_fast_objects)
BYPASS_SHORT_ACK_NARROW_STACK_OBJECTS = $(ack_port) $(lwip_fast_objects)

$(ack_source): bypass/stack.c bypass/ack-deadline.patch
	@mkdir -p "$(@D)"
	@set -eu; staged=$$(mktemp "$(@D)/stack.XXXXXXXX"); \
	    trap 'rm -f "$$staged"' EXIT; \
	    patch --batch --fuzz=0 --silent --reject-file=- --output="$$staged" "$<" < bypass/ack-deadline.patch; \
	    mv -f "$$staged" "$@"

$(ack_port): $(ack_source) bypass/ack-deadline.h bypass/stack.h bypass/lwipopts.h bypass/arch/cc.h challenge.h $(lwip_fast_stamp)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) -MMD -MP -c "$<" -o "$@"

bin/server-bypass-fast-wide-short-ack: $(app_objects) $(bypass_io) $(BYPASS_SHORT_ACK_STACK_OBJECTS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

bin/server-bypass-fast-short-ack: $(app_objects) $(bypass_io) $(BYPASS_SHORT_ACK_NARROW_STACK_OBJECTS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(ack_build)/test-wire-narrow: bypass/test-ack-wire.c $(BYPASS_SHORT_ACK_NARROW_STACK_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) $(LDFLAGS) -Wl,--wrap=clock_gettime $^ -o $@

.PHONY: check-ack-deadline-narrow
check-ack-deadline-narrow: $(ack_build)/test-queue $(ack_build)/test-wire-narrow
	$(ack_build)/test-queue
	$(ack_build)/test-wire-narrow

$(ack_build)/test-queue: bypass/test-ack-deadline.c bypass/ack-deadline.h | $(ack_source)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_flags) $(LDFLAGS) -UNDEBUG $< -o $@

$(ack_build)/test-wire: bypass/test-ack-wire.c $(BYPASS_SHORT_ACK_STACK_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) $(LDFLAGS) -Wl,--wrap=clock_gettime $^ -o $@

.PHONY: check-ack-deadline
check-ack-deadline: $(ack_build)/test-queue $(ack_build)/test-wire
	$(ack_build)/test-queue
	$(ack_build)/test-wire

-include $(ack_port:.o=.d)
endif
