/*
 * memwatch: insurance against running out of memory.
 *
 * The box has 2 GB and no swap, and a process the kernel kills loses the benchmark. Once a second this reads
 * MemAvailable and, when it is low, asks for a share of the responses to carry "Connection: close". A client
 * that gets one closes the connection and opens a new one for its next request (Go's HTTP client, k6, does
 * this by itself), so an idle user holds no socket in between; each socket is about 5 KB of kernel memory.
 *
 *   more than 1/4 of RAM available    nothing.
 *   1/4 down to 1/8                   0 to 100% of the responses close their connection, linearly.
 *   below 1/8                         every response closes, new connections wait in the kernel's queue,
 *                                     and idle connections are evicted at a bounded rate (conn_evict_idle): a
 *                                     client may lose a request that raced the eviction, which is better than
 *                                     the kernel's OOM killer.
 *
 * The window is wide because memory runs out fast: in the simulated 2 GB box (README.md, "Simulated target
 * environment") at 200,000 users MemAvailable fell from 233 MB to 156 MB in four seconds, and the kernel
 * killed the machine a second later. With the window starting at 1/8 the server reacted too late; with this
 * one the box bottoms out at 174 MB. With 175,000 users or fewer the lowest value was 562 MB, so nothing
 * here triggers.
 *
 * Shedding everything is not free: a reconnect costs the server about as much CPU as a request. That is
 * why the window is graded, and why the meter is MemAvailable alone: a second meter on TCP memory
 * (/proc/net/sockstat against net.ipv4.tcp_mem) read 62% of the kernel's pressure mark during the ramp,
 * shed 38% of the responses, and the churn pushed the kernel over the mark, where it spends 95% of the CPU
 * in system time.
 */
#ifndef MEMWATCH_H
#define MEMWATCH_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

enum {
	MEMWATCH_INTERVAL_SECONDS = 1,
	MEMWATCH_SHED_ALL = 256, /* shed level: out of 256 responses, how many carry "Connection: close" */
};

typedef enum memory_level {
	MEMORY_OK,
	MEMORY_LOW,      /* shedding some responses */
	MEMORY_CRITICAL, /* shedding all, evicting, and not accepting */
} memory_level;

typedef struct memwatch {
	const char *meminfo_path; /* /proc/meminfo; tests point it at a file of their own (SERVER_TEST_MEMINFO) */
	time_t last_check;
	memory_level level;
	unsigned shed;          /* 0 .. MEMWATCH_SHED_ALL */
	unsigned credit;        /* spreads the shed responses evenly */
	unsigned reported_shed; /* what the last log line said */
} memwatch;

void memwatch_init(memwatch *watch);

/* Re-reads the memory figures at most once per MEMWATCH_INTERVAL_SECONDS and returns the current level. */
memory_level memwatch_check(memwatch *watch, time_t now);

/* True when this response should carry "Connection: close": `shed` out of every 256 do. */
bool memwatch_should_shed(memwatch *watch);

/* The shed level (0 .. MEMWATCH_SHED_ALL) for a given MemAvailable out of MemTotal, both in kB. Exposed for tests. */
unsigned memwatch_shed_for(uint64_t available_kb, uint64_t total_kb);

#endif
