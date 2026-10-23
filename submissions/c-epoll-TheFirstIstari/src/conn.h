#pragma once
// Connection state, timing wheel, pools, eviction controller.
// Linux-only code lives in server.c; this header holds shared types.
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CONN_KEEPALIVE_S 75   // idle keep-alive (SPEC needs >= 65 behind nginx)
#define CONN_STALL_S     30   // no-progress with pending I/O -> close
#define WHEEL_BITS 8
#define WHEEL_SIZE (1u << WHEEL_BITS)

// FD-pressure tripwires (tuned against LimitNOFILE=65535; scaled if higher).
// Engage shedding at 60k, escalate at 63k, release at 55k (hysteresis).
#define EVICT_L1 60000
#define EVICT_L2 63000
#define EVICT_RELEASE 55000
// Memory shedding (usable MB): stop accept / close idle.
#define MEM_STOP_MB 192
#define MEM_SHED_MB 96

typedef struct conn conn_t;
struct conn {
  int fd;
  uint32_t last_act;    // seconds of last activity (wheel key base)
  uint32_t deadline;    // wheel expiry second (last_act + KEEPALIVE or stall)
  uint32_t req_since;   // request/stall start, seconds
  char *in;             // request accumulator (NULL while fully idle)
  size_t in_len, in_cap;
  char *out;            // framed response pending (NULL when none)
  size_t out_len, out_off;
  uint8_t closing;      // close after out drains
  uint8_t exp_sent;     // 100-continue already sent for current request
  uint8_t in_epollout;  // EPOLLOUT armed (partial send)
  uint8_t want_read;    // EPOLLIN currently armed
  uint8_t batch_resp;   // response built during open group-commit txn
  size_t pend_idx;    // index in server dirty/send list
  int in_pend;        // present in dirty/send list
  conn_t *wprev, *wnext;// timing-wheel links
};

typedef struct {
  conn_t *head, *tail;
} wheel_slot_t;

typedef struct {
  wheel_slot_t slot[WHEEL_SIZE];
  uint32_t tick;        // last processed second
} wheel_t;

typedef struct {
  long open_conns;      // self-tracked socket count (exact, O(1))
  int level;            // 0 normal, 1 pressure, 2 critical
  uint32_t last_shed;   // last shedding second (hysteresis cadence)
  uint64_t shed_win;    // current 250ms pacing window
  long shed_budget;     // sheds allowed in this window (token bucket)
  unsigned walk_from;   // rotating wheel start (bounded work per call)
  long fd_reserve;      // non-conn fds (listener, epoll, db, stdio...)
} evict_t;

// Implemented in server.c:
void wheel_init(wheel_t *w);
void wheel_arm(wheel_t *w, conn_t *c, uint32_t now_s, uint32_t in_secs);
void wheel_disarm(wheel_t *w, conn_t *c);
void wheel_tick(wheel_t *w, uint32_t now_s,
                void (*on_expire)(conn_t *c, void *arg), void *arg);
conn_t *conn_new(int fd, uint32_t now_s);
void conn_free(conn_t *c);
