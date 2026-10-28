AS = nasm
ASFLAGS = -f elf64 -Isrc -O3
CC = gcc
CFLAGS = -no-pie -O3
LDFLAGS = -lsqlite3 -lcrypto

SRCS = src/main.asm src/http.asm src/db.asm src/json.asm src/jwt.asm
OBJS = $(SRCS:.asm=.o)
TARGET = server

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

src/%.o: src/%.asm src/common.inc
	$(AS) $(ASFLAGS) $< -o $@

clean:
	rm -f src/*.o $(TARGET)

.PHONY: all clean
