# Build definition shared by build.sh (Ubuntu) and flake.nix (Nix).
#
#   make SQLITE_DIR=<dir with sqlite3.c and sqlite3.h>   builds bin/server
#   make test                                            builds and runs the unit tests
#   make SANITIZE=1 BUILD_DIR=build-san BIN_DIR=bin-san  the same under AddressSanitizer + UBSan
#   make clean
#
# Variables: CC, OPT (optimisation flags), EXTRA_CFLAGS (for example -Werror), SANITIZE, BUILD_DIR, BIN_DIR,
# COALESCE_FEEDS (0 = one feed query per request, the default; 1 = one shared feed query per event-loop round,
# which rule 5 may forbid: see src/batch.c).

SQLITE_DIR ?= .deps
URING_DIR  ?= .deps/liburing
URING_CFLAGS ?= -isystem $(URING_DIR)/src/include
URING_LIBS   ?= $(URING_DIR)/src/liburing.a
OPT        ?= -O2
COALESCE_FEEDS ?= 0
# How long a busy event loop lets work pile up between rounds, and the event rate above which it does.
PACER_WINDOW_US ?= 1000
PACER_BUSY_EVENTS_PER_SECOND ?= 3000
# SQLite maps this much of the database file (0 = read through its own cache), and keeps this much page cache.
STORE_MMAP_BYTES ?= 1073741824
STORE_CACHE_KIB ?= 16384
BUILD_DIR  ?= build
BIN_DIR    ?= bin

# Distribution compilers turn on stack protectors, FORTIFY_SOURCE, CET and PIE by default. In SQLite's
# bytecode interpreter they cost about 5%. This is a single-purpose server on a benchmark box, so they are
# switched off here, the same way for Ubuntu's gcc and for Nix's wrapper.
NO_HARDENING := -fno-stack-protector -U_FORTIFY_SOURCE -fcf-protection=none -fno-stack-clash-protection -fno-pie

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla
APP_FLAGS := $(OPT) -std=gnu11 -D_GNU_SOURCE -DCOALESCE_FEEDS=$(COALESCE_FEEDS) -DPACER_WINDOW_US=$(PACER_WINDOW_US) -DPACER_BUSY_EVENTS_PER_SECOND=$(PACER_BUSY_EVENTS_PER_SECOND) -DSTORE_MMAP_BYTES=$(STORE_MMAP_BYTES) -DSTORE_CACHE_KIB=$(STORE_CACHE_KIB) -fno-plt $(NO_HARDENING) $(WARNINGS) -Isrc $(URING_CFLAGS) $(EXTRA_CFLAGS)
ifdef SANITIZE
APP_FLAGS += -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
else
APP_FLAGS += -fomit-frame-pointer
endif

# SQLite is compiled for this one use: single thread, no extensions, no deprecated APIs.
# It never gets the warning or sanitizer flags: it is a pinned third-party file.
# Measured: -O3 is 4-5% faster than -O2 on the post and feed queries; profile-guided builds added under 1% more.
SQLITE_OPT ?= -O3
SQLITE_FLAGS := $(SQLITE_OPT) $(NO_HARDENING) -w \
	-DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
	-DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
	-DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTHORIZATION -DSQLITE_OMIT_DECLTYPE \
	-DSQLITE_OMIT_TRACE -DSQLITE_OMIT_JSON -DSQLITE_DQS=0 -DSQLITE_USE_ALLOCA \
	-DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_TEMP_STORE=2

HEADERS       := $(wildcard src/*.h)
LIB_SOURCES   := $(filter-out src/main.c,$(wildcard src/*.c)) # everything the unit tests link
LIB_OBJECTS   := $(LIB_SOURCES:src/%.c=$(BUILD_DIR)/%.o)
SQLITE_OBJECT := $(BUILD_DIR)/sqlite3.o
TEST_BINARIES := $(patsubst tests/unit/%.c,$(BUILD_DIR)/%,$(wildcard tests/unit/test_*.c))

.PHONY: all test clean
all: $(BIN_DIR)/server

$(BIN_DIR)/server: $(BUILD_DIR)/main.o $(LIB_OBJECTS) $(SQLITE_OBJECT) | $(BIN_DIR)
	$(CC) $(APP_FLAGS) -no-pie -o $@ $^ $(URING_LIBS)

$(SQLITE_OBJECT): $(SQLITE_DIR)/sqlite3.c | $(BUILD_DIR)
	$(CC) $(SQLITE_FLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: src/%.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(APP_FLAGS) -I$(SQLITE_DIR) -c -o $@ $<

$(BUILD_DIR)/test_%: tests/unit/test_%.c tests/unit/check.h $(LIB_OBJECTS) $(SQLITE_OBJECT)
	$(CC) $(APP_FLAGS) -I$(SQLITE_DIR) -Itests/unit -no-pie -o $@ $< $(LIB_OBJECTS) $(SQLITE_OBJECT) $(URING_LIBS)

test: $(TEST_BINARIES)
	@for t in $(TEST_BINARIES); do echo "== $$t"; "$$t" || exit 1; done

$(BIN_DIR) $(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf bin bin-san build build-san build-fuzz

# Not part of `all` or `test`: run it by hand, or through `nix run .#bench`.
$(BUILD_DIR)/query_bench: tests/bench/query_bench.c $(LIB_OBJECTS) $(SQLITE_OBJECT)
	$(CC) $(APP_FLAGS) -I$(SQLITE_DIR) -no-pie -o $@ $< $(LIB_OBJECTS) $(SQLITE_OBJECT) $(URING_LIBS)

# Fuzzing needs clang (libFuzzer): `make fuzz CC=clang FUZZ_SECONDS=60 SQLITE_DIR=...`
FUZZ_SOURCES := src/http_parse.c src/json_read.c src/jwt.c src/hmac_sha256.c src/sha256.c src/base64url.c src/utf8.c
FUZZ_TARGETS := $(patsubst tests/fuzz/%.c,%,$(wildcard tests/fuzz/fuzz_*.c))
FUZZ_SECONDS ?= 30

.PHONY: fuzz
fuzz:
	mkdir -p build-fuzz
	@for t in $(FUZZ_TARGETS); do \
		echo "== $$t"; \
		$(CC) -g -O1 -std=gnu11 -D_GNU_SOURCE -Isrc -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
			-o build-fuzz/$$t tests/fuzz/$$t.c $(FUZZ_SOURCES) || exit 1; \
		mkdir -p build-fuzz/corpus-$$t; \
		./build-fuzz/$$t -max_total_time=$(FUZZ_SECONDS) -timeout=5 -print_final_stats=1 build-fuzz/corpus-$$t 2>&1 | tail -4 || exit 1; \
	done
