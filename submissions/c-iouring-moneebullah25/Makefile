# Builds bin/server from src/ plus the pinned SQLite amalgamation and liburing
# in third_party/ (fetched by `make deps`, which build.sh runs first).
#
#   make            release build: -O3 -march=native, LTO (what build.sh ships)
#   make pgo        the same with PGO trained on a synthetic DB (opt-in: measured
#                   no CPU gain and lower throughput here, see docs/RESULTS.md)
#   make debug      -O0 -g
#   make asan       AddressSanitizer + UndefinedBehaviorSanitizer
#   make ubsan      UndefinedBehaviorSanitizer only
#   make check      unit tests (sanitized)
#   make fuzz       randomized mutation test of the parsers (sanitized)
#   make tools      load generator and synthetic database tool
#   make format     clang-format every source file
#   make lint       cppcheck, when installed

CC      ?= cc
JOBS    ?= 2
TP      := third_party
SQLITE_VER := 3540100
SQLITE_DIR := $(TP)/sqlite-amalgamation-$(SQLITE_VER)
URING_DIR  := $(TP)/liburing-liburing-2.15
URING_LIB  := $(URING_DIR)/src/liburing.a
SQLITE_SRC := $(SQLITE_DIR)/sqlite3.c

SRCS := src/main.c src/config.c src/net.c src/app.c src/db.c src/http.c src/json.c src/jwt.c src/crypto.c
TOOL_SRCS := src/http.c src/json.c src/jwt.c src/crypto.c

WARN := -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wvla -Wstrict-prototypes -Wmissing-prototypes \
        -Wno-unused-parameter
STD  := -std=gnu17 -D_GNU_SOURCE
INC  := -Isrc -isystem $(SQLITE_DIR) -isystem $(URING_DIR)/src/include
LIBS := -lpthread -lm

# SQLite: no mutexes on the connection (THREADSAFE=2 keeps the core mutexes
# that the checkpoint thread's second connection needs), no mmap, no stats.
SQLITE_DEFS := -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_AUTOINIT \
  -DSQLITE_USE_ALLOCA -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 \
  -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_MAX_MMAP_SIZE=0 -DHAVE_FDATASYNC=1 \
  -DSQLITE_TEMP_STORE=2 -DSQLITE_DEFAULT_FOREIGN_KEYS=0

OPT   := -O3 -march=native -fno-plt
LTO   := -flto=auto -fuse-linker-plugin
PROF  := build/prof

.PHONY: all release plain pgo debug asan ubsan check fuzz tools format lint clean deps

all: release

deps: $(SQLITE_SRC) $(URING_LIB)

$(SQLITE_SRC) $(URING_LIB):
	./fetch-deps.sh

build:
	mkdir -p build bin

# ---------------------------------------------------------------- release

release: plain

plain: deps build
	$(CC) $(STD) $(OPT) $(LTO) $(SQLITE_DEFS) -w -c $(SQLITE_SRC) -o build/sqlite3.o
	$(CC) $(STD) $(OPT) $(LTO) $(WARN) $(INC) $(SRCS) build/sqlite3.o $(URING_LIB) $(LIBS) -o bin/server

# ---------------------------------------------------------------- PGO release
# Stage 1 builds an instrumented server, stage 2 runs it against a synthetic
# database (never the seed, rule 7) under the load generator, stage 3 rebuilds
# with the profile. Any failure falls back to the plain build.

pgo: deps build tools
	@set -e; \
	rm -rf $(PROF); mkdir -p $(PROF); \
	GEN="-fprofile-generate -fprofile-dir=$(PROF) -fprofile-update=atomic"; \
	USE="-fprofile-use -fprofile-dir=$(PROF) -fprofile-correction -Wno-missing-profile"; \
	echo "pgo: instrumented build"; \
	$(CC) $(STD) $(OPT) $(SQLITE_DEFS) $$GEN -w -c $(SQLITE_SRC) -o build/sqlite3_pgo.o; \
	$(CC) $(STD) $(OPT) $(WARN) $(INC) $$GEN $(SRCS) build/sqlite3_pgo.o $(URING_LIB) $(LIBS) -o build/server_pgo; \
	echo "pgo: training run"; \
	if ./pgo/train.sh build/server_pgo; then \
	  echo "pgo: optimized build"; \
	  $(CC) $(STD) $(OPT) $(LTO) $(SQLITE_DEFS) $$USE -w -c $(SQLITE_SRC) -o build/sqlite3.o; \
	  $(CC) $(STD) $(OPT) $(LTO) $(WARN) $(INC) $$USE $(SRCS) build/sqlite3.o $(URING_LIB) $(LIBS) -o bin/server; \
	else \
	  echo "pgo: training failed, building without a profile"; \
	  $(MAKE) plain; \
	fi

# ---------------------------------------------------------------- debug / sanitizers

debug: deps build
	$(CC) $(STD) -O0 -g $(SQLITE_DEFS) -DSQLITE_DEBUG -w -c $(SQLITE_SRC) -o build/sqlite3_dbg.o
	$(CC) $(STD) -O0 -g $(WARN) $(INC) $(SRCS) build/sqlite3_dbg.o $(URING_LIB) $(LIBS) -o bin/server-debug

asan: deps build
	$(CC) $(STD) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(SQLITE_DEFS) -w -c $(SQLITE_SRC) -o build/sqlite3_asan.o
	$(CC) $(STD) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(WARN) $(INC) $(SRCS) build/sqlite3_asan.o $(URING_LIB) $(LIBS) -o bin/server-asan

ubsan: deps build
	$(CC) $(STD) -O1 -g -fsanitize=undefined -fno-omit-frame-pointer $(SQLITE_DEFS) -w -c $(SQLITE_SRC) -o build/sqlite3_ubsan.o
	$(CC) $(STD) -O1 -g -fsanitize=undefined -fno-omit-frame-pointer $(WARN) $(INC) $(SRCS) build/sqlite3_ubsan.o $(URING_LIB) $(LIBS) -o bin/server-ubsan

# ---------------------------------------------------------------- tests and tools

check: build
	$(CC) $(STD) -O1 -g -fsanitize=address,undefined $(WARN) -Isrc test/unit_tests.c $(TOOL_SRCS) -o build/unit_tests
	./build/unit_tests

fuzz: build
	$(CC) $(STD) -O1 -g -fsanitize=address,undefined $(WARN) -Isrc test/fuzz.c $(TOOL_SRCS) -o build/fuzz
	./build/fuzz $(FUZZ_ITERS)

tools: deps build
	$(CC) $(STD) -O2 $(WARN) -Isrc tools/mload.c src/jwt.c src/crypto.c src/json.c $(LIBS) -o bin/mload
	$(CC) $(STD) -O2 $(SQLITE_DEFS) -w -c $(SQLITE_SRC) -o build/sqlite3_tools.o
	$(CC) $(STD) -O2 $(WARN) $(INC) tools/synthdb.c src/jwt.c src/crypto.c src/json.c build/sqlite3_tools.o $(LIBS) -o bin/synthdb

format:
	clang-format -i src/*.c src/*.h test/*.c tools/*.c

lint:
	@command -v cppcheck >/dev/null && cppcheck --std=c11 --enable=warning,performance,portability \
	  --suppress=missingIncludeSystem --inline-suppr -Isrc src/*.c tools/*.c test/*.c || echo "cppcheck not installed"

clean:
	rm -rf build bin
