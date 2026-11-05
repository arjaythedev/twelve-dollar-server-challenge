# The Dreamcast program. Needs a KallistiOS environment (source build/kos/environ.sh) and
# SQLITE_DIR pointing at the SQLite amalgamation; build.sh does both.
TARGET = dreamcast-sql.elf
OBJS = main.o api.o vfs_hostfs.o jwt.o sqlite3.o

SQLITE_DIR ?= ../build/sqlite
SQLITE_OPTS = -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_TEMP_STORE=3 \
	-DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
	-DSQLITE_OMIT_SHARED_CACHE -DSQLITE_DQS=0 -DSQLITE_MAX_MMAP_SIZE=0

KOS_CFLAGS += -I. -Ithird_party -I$(SQLITE_DIR) -Wall

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

sqlite3.o: $(SQLITE_DIR)/sqlite3.c
	kos-cc $(KOS_CFLAGS) $(SQLITE_OPTS) -Wno-all -c $< -o $@

jwt.o: third_party/jwt.c
	kos-cc $(KOS_CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS)

clean:
	rm -f $(TARGET) $(OBJS)
