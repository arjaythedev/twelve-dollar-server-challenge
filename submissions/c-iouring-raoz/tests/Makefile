# Test programs. Not part of the submission build: build.sh never builds or runs them.
# Run build.sh first (it fetches the pinned SQLite and liburing sources and builds bin/server), then
#   make -C tests check
# Each program starts its own server on a free port, with a fresh copy of seed/feed.db.

CC ?= gcc
CFLAGS = -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-function
B = build
SERVER_SRC = ../src/server.c ../src/json.h ../src/sha256.h ../src/train.h
INC = -I../deps/sqlite -I../deps/liburing/src/include
LIBURING = ../deps/liburing/src/liburing.a
SQLITE_OPTS = -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DQS=0 \
  -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK \
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_USE_ALLOCA

CLIENTS = $(B)/api $(B)/http $(B)/commit $(B)/lifecycle
all: $(CLIENTS) $(B)/parser $(B)/commit-server $(B)/loadgen

$(B):
	mkdir -p $(B)

$(CLIENTS): $(B)/%: %.c t.h ../src/sha256.h | $(B)
	$(CC) $(CFLAGS) $< -o $@

# The load generator for capacity tests (see loadgen.c); built here, not run by check.
$(B)/loadgen: loadgen.c | $(B)
	$(CC) $(CFLAGS) -pthread $< -lm -o $@

$(B)/sqlite3.o: ../deps/sqlite/sqlite3.c | $(B)
	$(CC) -O2 $(SQLITE_OPTS) -w -c $< -o $@

# The request parser in-process, under AddressSanitizer and UBSan.
$(B)/parser: parser.c $(SERVER_SRC) $(B)/sqlite3.o
	$(CC) -O1 -g -std=gnu11 -fsanitize=address,undefined -fno-omit-frame-pointer $(INC) $< $(B)/sqlite3.o $(LIBURING) -lpthread -lm -o $@

# The server with a commit hook that can fail or stall commits on request (see commit_fault.c).
$(B)/commit-server: commit_fault.c $(SERVER_SRC) $(B)/sqlite3.o
	$(CC) -O2 -std=gnu11 -w $(INC) ../src/server.c commit_fault.c $(B)/sqlite3.o $(LIBURING) \
	  -Wl,--wrap=sqlite3_open_v2 -lpthread -lm -o $@

check: all
	ASAN_OPTIONS=detect_leaks=0 ./$(B)/parser
	./$(B)/api
	./$(B)/http
	SERVER=$(B)/commit-server ./$(B)/commit
	./$(B)/lifecycle

clean:
	rm -rf $(B)

.PHONY: all check clean
