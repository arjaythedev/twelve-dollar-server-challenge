# Stack layout, enrollment and object files are private to this testable variant.
ifndef TWELVE_LWIP_SLOW_MK
TWELVE_LWIP_SLOW_MK := 1
LWIP_SLOW_BASE_DIR ?= $(lwip_fast_dir)
LWIP_SLOW_BASE_STAMP ?= $(lwip_fast_stamp)
LWIP_SLOW_BASE_KEY ?= $(lwip_index_key)-$(lwip_fast_key)
LWIP_SLOW_PORT_SOURCE ?= bypass/stack.c
LWIP_SLOW_CPPFLAGS = -DLWIP_TCP_TUPLE_INDEX_BITS=17 -DLWIP_TCP_FAST_WORK=1 -DLWIP_TCP_SLOW_WORK=1
LWIP_SLOW_PORT_FLAGS = -include bypass/slow-port.h
lwip_slow_patch = bypass/lwip-slow-work.patch
lwip_slow_key := $(word 1,$(shell if test -f bypass/lwip-slow-work.patch; then sha256sum bypass/lwip-slow-work.patch; fi))
lwip_slow_build = $(BUILD)/bypass/slow-$(LWIP_SLOW_BASE_KEY)-$(lwip_slow_key)
lwip_slow_dir = $(lwip_slow_build)/source
lwip_slow_stamp = $(lwip_slow_dir)/.index-ready
lwip_slow_objects = $(addprefix $(lwip_slow_build)/lwip/,$(addsuffix .o,$(lwip_units)))
lwip_slow_flags = -Ibypass -I$(lwip_slow_dir)/src/include $(LWIP_SLOW_CPPFLAGS)
BYPASS_SLOW_STACK_OBJECTS = $(lwip_slow_build)/stack.o $(lwip_slow_objects)

$(lwip_slow_stamp): $(lwip_slow_patch) $(LWIP_SLOW_BASE_STAMP) bypass/index-lwip.sh
	bash bypass/index-lwip.sh "$(LWIP_SLOW_BASE_DIR)" "$(lwip_slow_dir)" "$(lwip_slow_patch)"

$(lwip_slow_objects): $(lwip_slow_build)/lwip/%.o: $(lwip_slow_stamp) bypass/lwipopts.h bypass/arch/cc.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_slow_flags) -MMD -MP -c "$(lwip_slow_dir)/src/$*.c" -o "$@"

$(lwip_slow_build)/stack.o: $(LWIP_SLOW_PORT_SOURCE) bypass/stack.h bypass/slow-port.h bypass/lwipopts.h bypass/arch/cc.h challenge.h $(lwip_slow_stamp)
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_slow_flags) $(LWIP_SLOW_PORT_FLAGS) -MMD -MP -c "$<" -o "$@"

bin/server-bypass-slow: $(app_objects) $(bypass_io) $(BYPASS_SLOW_STACK_OBJECTS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BYPASS_SLOW_STACK_OBJECTS:.o=.d): ;
-include $(wildcard $(BYPASS_SLOW_STACK_OBJECTS:.o=.d))
endif
