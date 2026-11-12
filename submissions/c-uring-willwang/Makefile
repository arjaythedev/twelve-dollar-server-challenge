CC = clang-18
CPPFLAGS = -D_GNU_SOURCE -DNDEBUG -I. -Ivendor
CFLAGS = -O3 -march=native -fno-plt -std=gnu11 -flto
LDFLAGS = -fuse-ld=gold -flto
LDLIBS = -lcrypto -pthread -ldl -lm
SQLITE_FLAGS = -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
               -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION \
               -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_USE_ALLOCA -DHAVE_FDATASYNC=1
BUILD ?= build
app_objects = $(addprefix $(BUILD)/,buffer.o database.o wal.o reader.o app.o)
objects = $(BUILD)/main.o $(app_objects) $(BUILD)/reactor.o
all: bin/server bin/server-bypass bin/server-bypass-fixed
bin/server: $(objects) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
include bypass/stack.mk
bypass_io = $(addprefix $(BUILD)/bypass/,main.o link.o link_xdp.o local.o)
$(bypass_io): CPPFLAGS += -DTWELVE_AF_XDP
include bypass/ack-output.mk
include bypass/fixed-link.mk
bin/server-bypass bin/server-bypass-fast: $(app_objects) $(bypass_io) $(BYPASS_FAST_STACK_OBJECTS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
bin/server-bypass-stock: $(app_objects) $(bypass_io) $(BYPASS_STACK_OBJECTS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
bin/server-bypass-indexed: $(app_objects) $(bypass_io) $(BYPASS_INDEXED_STACK_OBJECTS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(BUILD)/bypass/%.o: bypass/%.c Makefile
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -Ibypass -MMD -MP -c $< -o $@
$(BUILD)/bypass/local.o: reactor.c bypass/local.h Makefile
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTWELVE_LOCAL_REACTOR -MMD -MP -c $< -o $@
$(BUILD)/reader.o: CPPFLAGS += $(SQLITE_FLAGS)
$(BUILD)/%.o: %.c Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD) bin:
	mkdir -p $@
-include $(objects:.o=.d) $(bypass_io:.o=.d)
.PHONY: all
