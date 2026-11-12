LWIP_DIR ?= vendor/lwip-2.2.1
lwip_units = core/init core/def core/inet_chksum core/ip core/mem core/memp \
             core/netif core/pbuf core/stats core/sys core/tcp core/tcp_in \
             core/tcp_out core/timeouts core/ipv4/etharp core/ipv4/icmp \
             core/ipv4/ip4 core/ipv4/ip4_addr core/ipv4/ip4_frag netif/ethernet
lwip_objects = $(addprefix $(BUILD)/bypass/lwip/,$(addsuffix .o,$(lwip_units)))
BYPASS_STACK_OBJECTS = $(BUILD)/bypass/stack.o $(lwip_objects)
lwip_flags = -Ibypass -I$(LWIP_DIR)/src/include

$(LWIP_DIR)/src/include/lwip/init.h:
	bash bypass/fetch-lwip.sh "$(LWIP_DIR)"

.PRECIOUS: $(LWIP_DIR)/src/%.c
$(LWIP_DIR)/src/%.c: | $(LWIP_DIR)/src/include/lwip/init.h
	@test -f "$@"

$(lwip_objects): $(BUILD)/bypass/lwip/%.o: $(LWIP_DIR)/src/%.c bypass/lwipopts.h bypass/arch/cc.h | $(LWIP_DIR)/src/include/lwip/init.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_flags) -MMD -MP -c "$<" -o "$@"

$(BUILD)/bypass/stack.o: bypass/stack.c bypass/stack.h bypass/lwipopts.h bypass/arch/cc.h challenge.h | $(LWIP_DIR)/src/include/lwip/init.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_flags) -MMD -MP -c "$<" -o "$@"

$(BYPASS_STACK_OBJECTS:.o=.d): ;
-include $(wildcard $(BYPASS_STACK_OBJECTS:.o=.d))

# Control and candidate have incompatible tcp_pcb layouts. Keep independent
# source trees and object lists, even when building both in one make invocation.
lwip_index_patch = bypass/lwip-pcb-index.patch
lwip_index_key := $(word 1,$(shell if test -f bypass/lwip-pcb-index.patch; then sha256sum bypass/lwip-pcb-index.patch; fi))
lwip_index_dir = $(BUILD)/bypass/lwip-2.2.1-index-$(lwip_index_key)
lwip_index_stamp = $(lwip_index_dir)/.index-ready
lwip_index_objects = $(addprefix $(BUILD)/bypass/indexed-lwip/,$(addsuffix .o,$(lwip_units)))
BYPASS_INDEXED_STACK_OBJECTS = $(BUILD)/bypass/indexed-stack.o $(lwip_index_objects)
lwip_index_flags = -Ibypass -I$(lwip_index_dir)/src/include -DLWIP_TCP_TUPLE_INDEX_BITS=17

$(lwip_index_stamp): $(lwip_index_patch) bypass/index-lwip.sh | $(LWIP_DIR)/src/include/lwip/init.h
	bash bypass/index-lwip.sh "$(LWIP_DIR)" "$(lwip_index_dir)" "$(lwip_index_patch)"

$(lwip_index_objects): $(BUILD)/bypass/indexed-lwip/%.o: $(lwip_index_stamp) bypass/lwipopts.h bypass/arch/cc.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_index_flags) -MMD -MP -c "$(lwip_index_dir)/src/$*.c" -o "$@"

$(BUILD)/bypass/indexed-stack.o: bypass/stack.c bypass/stack.h bypass/lwipopts.h bypass/arch/cc.h challenge.h $(lwip_index_stamp)
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_index_flags) -MMD -MP -c "$<" -o "$@"

$(BYPASS_INDEXED_STACK_OBJECTS:.o=.d): ;
-include $(wildcard $(BYPASS_INDEXED_STACK_OBJECTS:.o=.d))

# The release fast timer changes the PCB layout again. Key its complete
# source and object directory by both patches so it can coexist with controls.
lwip_fast_patch = bypass/lwip-fast-work.patch
lwip_fast_key := $(word 1,$(shell if test -f bypass/lwip-fast-work.patch; then sha256sum bypass/lwip-fast-work.patch; fi))
lwip_fast_build = $(BUILD)/bypass/fast-$(lwip_index_key)-$(lwip_fast_key)
lwip_fast_dir = $(lwip_fast_build)/source
lwip_fast_stamp = $(lwip_fast_dir)/.index-ready
lwip_fast_objects = $(addprefix $(lwip_fast_build)/lwip/,$(addsuffix .o,$(lwip_units)))
BYPASS_FAST_STACK_OBJECTS = $(lwip_fast_build)/stack.o $(lwip_fast_objects)
lwip_fast_flags = -Ibypass -I$(lwip_fast_dir)/src/include -DLWIP_TCP_TUPLE_INDEX_BITS=17 -DLWIP_TCP_FAST_WORK=1

$(lwip_fast_stamp): $(lwip_fast_patch) $(lwip_index_stamp) bypass/index-lwip.sh
	bash bypass/index-lwip.sh "$(lwip_index_dir)" "$(lwip_fast_dir)" "$(lwip_fast_patch)"

$(lwip_fast_objects): $(lwip_fast_build)/lwip/%.o: $(lwip_fast_stamp) bypass/lwipopts.h bypass/arch/cc.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) -MMD -MP -c "$(lwip_fast_dir)/src/$*.c" -o "$@"

$(lwip_fast_build)/stack.o: bypass/stack.c bypass/stack.h bypass/lwipopts.h bypass/arch/cc.h challenge.h $(lwip_fast_stamp)
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_fast_flags) -MMD -MP -c "$<" -o "$@"

$(BYPASS_FAST_STACK_OBJECTS:.o=.d): ;
-include $(wildcard $(BYPASS_FAST_STACK_OBJECTS:.o=.d))
