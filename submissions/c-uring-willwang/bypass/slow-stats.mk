# Optional diagnostic overlay. Only tcp.c and the port reporter need distinct objects.
ifndef TWELVE_LWIP_SLOW_STATS_MK
TWELVE_LWIP_SLOW_STATS_MK := 1
include bypass/slow.mk
LWIP_SLOW_STATS_BASE_DIR ?= $(lwip_slow_dir)
LWIP_SLOW_STATS_BASE_STAMP ?= $(lwip_slow_stamp)
LWIP_SLOW_STATS_BASE_KEY ?= $(lwip_slow_key)
LWIP_SLOW_STATS_OTHER_OBJECTS ?= $(filter-out $(lwip_slow_build)/lwip/core/tcp.o,$(lwip_slow_objects))
LWIP_SLOW_STATS_PORT_SOURCE ?= $(LWIP_SLOW_PORT_SOURCE)
LWIP_SLOW_STATS_CPPFLAGS = -DLWIP_TCP_SLOW_STATS=1
LWIP_SLOW_STATS_PORT_FLAGS = $(LWIP_SLOW_PORT_FLAGS) -Dstack_tail_report=stack_calendar_original_tail_report
lwip_slow_stats_patch = bypass/lwip-slow-stats.patch
lwip_slow_stats_key := $(word 1,$(shell if test -f bypass/lwip-slow-stats.patch; then sha256sum bypass/lwip-slow-stats.patch; fi))
lwip_slow_stats_build = $(BUILD)/bypass/slow-stats-$(LWIP_SLOW_STATS_BASE_KEY)-$(lwip_slow_stats_key)
lwip_slow_stats_dir = $(lwip_slow_stats_build)/source
lwip_slow_stats_stamp = $(lwip_slow_stats_dir)/.index-ready
lwip_slow_stats_flags = -Ibypass -I$(lwip_slow_stats_dir)/src/include $(LWIP_SLOW_CPPFLAGS) $(LWIP_SLOW_STATS_CPPFLAGS)
BYPASS_SLOW_STATS_STACK_OBJECTS = $(lwip_slow_stats_build)/port.o $(lwip_slow_stats_build)/report.o $(lwip_slow_stats_build)/tcp.o $(LWIP_SLOW_STATS_OTHER_OBJECTS)

$(lwip_slow_stats_stamp): $(lwip_slow_stats_patch) $(LWIP_SLOW_STATS_BASE_STAMP) bypass/index-lwip.sh
	bash bypass/index-lwip.sh "$(LWIP_SLOW_STATS_BASE_DIR)" "$(lwip_slow_stats_dir)" "$(lwip_slow_stats_patch)"

$(lwip_slow_stats_build)/tcp.o: $(lwip_slow_stats_stamp) bypass/slow-stats.h bypass/lwipopts.h bypass/arch/cc.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_slow_stats_flags) -MMD -MP -c "$(lwip_slow_stats_dir)/src/core/tcp.c" -o "$@"

$(lwip_slow_stats_build)/port.o: $(LWIP_SLOW_STATS_PORT_SOURCE) bypass/stack.h bypass/slow-port.h bypass/lwipopts.h bypass/arch/cc.h challenge.h $(lwip_slow_stats_stamp)
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(lwip_slow_stats_flags) $(LWIP_SLOW_STATS_PORT_FLAGS) -MMD -MP -c "$<" -o "$@"

$(lwip_slow_stats_build)/report.o: bypass/slow-stats.c bypass/slow-stats.h bypass/stack.h
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -Ibypass -MMD -MP -c "$<" -o "$@"

bin/server-bypass-slow-stats: $(app_objects) $(bypass_io) $(BYPASS_SLOW_STATS_STACK_OBJECTS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

lwip_slow_stats_deps = $(addprefix $(lwip_slow_stats_build)/,tcp.d port.d report.d)
$(lwip_slow_stats_deps): ;
-include $(wildcard $(lwip_slow_stats_deps))
endif
