CC = clang-18
CPPFLAGS = -D_GNU_SOURCE -DNDEBUG -I. -Ivendor
CFLAGS = -O3 -march=native -fno-plt -std=gnu11 -flto
LDFLAGS = -fuse-ld=gold -flto
LDLIBS = -lcrypto -pthread -ldl -lm
SQLITE_FLAGS = -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
               -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION \
               -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_USE_ALLOCA -DHAVE_FDATASYNC=1
BUILD ?= build
objects = $(addprefix $(BUILD)/,main.o buffer.o database.o wal.o reader.o app.o reactor.o)
all: bin/server
bin/server: $(objects) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(BUILD)/reader.o: CPPFLAGS += $(SQLITE_FLAGS)
$(BUILD)/%.o: %.c Makefile | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD) bin:
	mkdir -p $@
-include $(objects:.o=.d)
.PHONY: all
