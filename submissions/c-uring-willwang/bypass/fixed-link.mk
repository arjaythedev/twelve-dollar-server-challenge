# Rebuild the previously measured fixed 512-slot link without changing
# the adjustable-link sources used by the other Ethernet experiments.
fixed_inputs = challenge.h $(addprefix bypass/,main.c link.c link_xdp.c link.h link_xdp.h packet_gso.h local.h stack.h)
fixed_patch = bypass/fixed-link.patch
fixed_key := $(firstword $(shell sha256sum $(fixed_inputs) $(fixed_patch) | sha256sum))
fixed_build = $(BUILD)/bypass/fixed-$(fixed_key)
fixed_io = $(addprefix $(fixed_build)/,main.o link.o link_xdp.o)
fixed_stamp = $(fixed_build)/.ready

$(fixed_stamp): $(fixed_inputs) $(fixed_patch)
	@set -eu; parent="$(dir $(@D))"; mkdir -p "$$parent"; stage=$$(mktemp -d "$$parent/fixed-link.XXXXXXXX"); \
	    trap 'rm -rf -- "$$stage"' EXIT; \
	    cp --parents $(fixed_inputs) "$$stage"; \
	    patch --batch --fuzz=0 --silent -p1 -d "$$stage" < "$(fixed_patch)"; \
	    touch "$$stage/.ready"; \
	    if ! mv -T "$$stage" "$(@D)" 2>/dev/null; then test -f "$@"; fi

# Use the measured compilation-unit paths. link_xdp.c includes the fixed
# LinkConfig header as well, so all three units agree on struct layout.
$(fixed_io): $(fixed_build)/%.o: $(fixed_stamp) Makefile bypass/fixed-link.mk
	cd "$(@D)" && $(CC) $(CPPFLAGS) -DTWELVE_AF_XDP $(CFLAGS) -Ibypass -c "bypass/$*.c" -o "$(@F)"

bin/server-bypass-fixed: $(app_objects) $(fixed_io) $(BUILD)/bypass/local.o $(BYPASS_FAST_STACK_OBJECTS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
