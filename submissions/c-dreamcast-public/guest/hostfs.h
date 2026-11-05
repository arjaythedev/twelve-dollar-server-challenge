/* hostfs.h - the guest side of the hostfs "BIOS" syscall that our patched Flycast puts at
 * 0x8C00100A (flycast/core/reios/hostfs.cpp has the host side and the op list).
 * Every op takes a block of u32 arguments; the emulator reads and writes it in place. */
#pragma once
#include <stdint.h>

enum { HFS_OPEN, HFS_CLOSE, HFS_PREAD, HFS_PWRITE, HFS_SIZE, HFS_TRUNCATE, HFS_SYNC,
       HFS_DELETE, HFS_ACCESS, HFS_GETENV, HFS_TIME, HFS_LOG };

#define HOSTFS_ENTRY 0x8C00100Au
#define HOSTFS_MAGIC 0x31534648u /* 'HFS1' at HOSTFS_ENTRY + 6 */

static inline int hostfs_present(void) {
    return *(volatile uint32_t *)(HOSTFS_ENTRY + 6) == HOSTFS_MAGIC;
}

static inline int32_t hostfs(uint32_t op, volatile uint32_t *blk) {
    return ((int32_t (*)(uint32_t, volatile uint32_t *))HOSTFS_ENTRY)(op, blk);
}

static inline uint64_t hostfs_time_ms(void) {
    volatile uint32_t b[4] = {0};
    hostfs(HFS_TIME, b);
    return ((uint64_t)b[1] << 32) | b[0];
}

static inline void hostfs_log(const char *s) {
    volatile uint32_t b[4] = {(uint32_t)s};
    hostfs(HFS_LOG, b);
}

static inline int hostfs_getenv(const char *name, char *buf, uint32_t len) {
    volatile uint32_t b[4] = {(uint32_t)name, (uint32_t)buf, len};
    return hostfs(HFS_GETENV, b);
}
