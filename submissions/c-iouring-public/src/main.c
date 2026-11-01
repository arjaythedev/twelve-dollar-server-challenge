// main.c — process setup: env config (SPEC.md runtime contract), rlimits, signals, engine + loop.
#include "app.h"
#include "loop.h"
#include "engine.h"
#include "config.h"
const char *sqlite3_libversion(void);
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_shed_mode = "band";  // stats line: which shedding law runs
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>

int train_main(int argc, char **argv);  // train.c: PGO training driver (build time only)

static loop_t *gL;
// SIGINT/SIGTERM: stop once no file ops are in flight (DB_CKPT=uring); completions are only reaped while the loop runs
static volatile sig_atomic_t stopping;
static void on_sig(int s) {
  (void)s;
  stopping = 1;
#if APP_DB_CKPT != 3
  if (gL) loop_stop(gL);
#endif
}

static int on_batch(loop_t *L, loop_in *in, int n, void *ud) {
  (void)ud;
  return app_batch(L, in, n);
}

static int stats_every;
static void on_tick(loop_t *L, void *ud) {
  (void)ud;
  if (stopping) app_quiesce();
  app_tick(L);
  if (stopping && !app_inflight()) loop_stop(L);
  if (stats_every) {
    static time_t last;
    time_t t = time(NULL);
    if (t - last >= stats_every) {
      last = t;
      const loop_stats *s = loop_get_stats(L);
      const app_stats *a = app_get_stats();
      struct rusage ru;
      getrusage(RUSAGE_SELF, &ru);
      eng_stats es;
      eng_get_stats(app_engine(), &es);
      fprintf(stderr,
              "engine ckpts=%llu ckpt_frames=%llu ckpt_max_ms=%.1f ckpt_total_ms=%.0f wal_max=%llu drains=%llu "
              "drain_timeouts=%llu "
              "drain_ms_max=%.1f busy_sleeps=%llu restarts=%llu wal_resets=%llu wal_dropped=%lluMB wal_flushes=%llu "
              "commits_failed=%llu turbo=%llu turbo_fallbacks=%llu\n",
              (unsigned long long)es.checkpoints, (unsigned long long)es.ckpt_frames, es.ckpt_max_ms, es.ckpt_total_ms,
              (unsigned long long)es.wal_max, (unsigned long long)es.drains, (unsigned long long)es.drains_timed_out,
              es.drain_ms_max, (unsigned long long)es.busy_sleeps, (unsigned long long)es.restarts,
              (unsigned long long)es.wal_resets, (unsigned long long)(es.wal_dropped_bytes >> 20),
              (unsigned long long)es.wal_flushes, (unsigned long long)es.commits_failed,
              (unsigned long long)es.turbo_served, (unsigned long long)es.turbo_fallbacks);
      fprintf(stderr,
              "stats shed=%s open=%u acc=%llu req=%llu batches=%llu lanes=%llu slow=%llu slow_checked=%llu jwt=%llu held=%llu "
              "cfail=%llu deadline_503=%llu press_503=%llu sock_pressure=%u/%llu all_503=%llu b503_skipped=%llu b503_tokens=%llu "
              "hint_closes=%llu close_frac=%.2f cap=%u busy=%.2f avail=%uMB orphans=%u listen_overflows=%llu "
              "listen_drops=%llu "
              "backlog=%u busy_src=%d mem_state=%u accepting=%d enobufs=%llu rearms=%llu loop_drains=%llu enter_errs=%llu accept_ends=%llu tcp_mem=%u utime=%ld.%06ld stime=%ld.%06ld maxrss=%ldKB\n",
              g_shed_mode, s->open, (unsigned long long)s->accepted, (unsigned long long)a->requests, (unsigned long long)a->batches,
              (unsigned long long)a->lanes, (unsigned long long)a->slow, (unsigned long long)a->slow_checked,
              (unsigned long long)a->jwts, (unsigned long long)a->held, (unsigned long long)a->commit_failures,
              (unsigned long long)a->deadline_503, (unsigned long long)a->press_503, s->sock_pressure, (unsigned long long)s->sock_pressure_events, (unsigned long long)a->all_503, (unsigned long long)a->b503_skipped, (unsigned long long)a->b503_tokens, (unsigned long long)s->hint_closes, s->close_frac, s->cap, s->busy,
              s->avail_mb, s->tcp_orphans, (unsigned long long)s->listen_overflows, (unsigned long long)s->listen_drops,
              s->backlog_now, s->busy_src, s->mem_state, s->accepting, (unsigned long long)s->enobufs, (unsigned long long)s->rearms,
              (unsigned long long)s->drains, (unsigned long long)s->enter_errs, (unsigned long long)s->accept_ends, s->tcp_mem_pages, (long)ru.ru_utime.tv_sec, (long)ru.ru_utime.tv_usec,
              (long)ru.ru_stime.tv_sec, (long)ru.ru_stime.tv_usec, ru.ru_maxrss);
    }
  }
}

static unsigned envu(const char *k, unsigned d) {
  const char *v = getenv(k);
  return v && *v ? (unsigned)strtoul(v, 0, 10) : d;
}
static float permille(unsigned v) { return (float)v / 1000.0f; }

// Lock the read-only file-backed mappings (the server's code and rodata, libc, the loader: ~3.5 MB, inside the box's
// 8 MB RLIMIT_MEMLOCK) into RAM. At the memory wall the kernel evicts them like any page cache, the loop then takes
// major faults that read its own code back from disk under reclaim, and a stall of a second or two leaves every
// connection's next request unread in the kernel (~1.3 KB each), which is what tips the box into the OOM killer
// (wall-280k: 28 major faults in the stall that took TCP memory to 76 MB at 60k connections). Best effort.
static size_t lock_code(void) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) return 0;
  char line[512];
  size_t locked = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long lo, hi;
    char perms[8];
    int path = 0;
    if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &path) < 3 || !path || line[path] != '/') continue;
    if (perms[1] == 'w' || strstr(line + path, ".db")) continue;   // code and rodata only, never the database
    if (!mlock((void *)lo, hi - lo)) locked += hi - lo;
  }
  fclose(f);
  return locked;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--train")) return train_main(argc - 1, argv + 1);
  const char *db = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET");
  if (!db || !secret) {
    fprintf(stderr, "server: SQLITE_PATH and JWT_SECRET are required\n");
    return 2;
  }
  struct rlimit rl;  // raise the soft fd limit to the hard one (65,535 on the box)
  if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }

  char err[256];
  if (app_init(db, secret, err, sizeof err)) {
    fprintf(stderr, "server: %s\n", err);
    return 1;
  }
  // INTEGRATION.md §2 defaults. CURSED_* overrides exist for experiments only; start.sh sets none.
  // deadline shedding needs the kernel's receive timestamps, so CURSED_DEADLINE_MS alone turns them on
  const char *dl = getenv("CURSED_DEADLINE_MS");
  int deadline_on = dl && atoi(dl) > 0;
  loop_cfg cfg = {
      .host = getenv("HOST") && *getenv("HOST") ? getenv("HOST") : "0.0.0.0",
      .port = (int)envu("PORT", 80),
      // like Lean #153: 8 SO_REUSEPORT listeners at listen(65535) (clamped to somaxconn, 4096 on the box), so the
      // accept queues hold ~32k connections through a loop stall instead of dropping SYNs (1 s+ retransmits)
      .backlog = (int)envu("CURSED_BACKLOG", 65535),
      .listeners = (int)envu("CURSED_LISTENERS", 8),
      .cq_entries = envu("CURSED_CQ", 32768),
      // receive buffers per ring, and multishot re-arms per pass after ENOBUFS (0: no limit). 1,024 / 0 (Lean's)
      // didn't change how often a stall at the wall turned fatal (1 of 5 vs 2 of 5, wall-280k), so 512 / 256 stay
      .bufs = envu("CURSED_BUFS", 512),
      .rearm_credit = envu("CURSED_REARM_CREDIT", 256),
      .defer_accept = (int)envu("CURSED_DEFER_ACCEPT", 0),
      .linger_close = (int)strtol(getenv("CURSED_LINGER") ? getenv("CURSED_LINGER") : "0", 0, 10),
      .batch = envu("CURSED_BATCH", 128),
      .wait_us = envu("CURSED_WAIT_US", 2000),
      // with 2+ rings (65,535 connections each) ring 0's wait is capped at poll_wait_us and counts only its own
      // completions, so this, not CURSED_BATCH, sets the batch size at 130k+ connections (~24 requests at 400k)
      .poll_wait_us = envu("CURSED_POLL_WAIT_US", 1000),
      .conn_policy = (int)envu("CURSED_CONN_POLICY", 0),   // closes off: every reconnect costs more than it saves (arena, 450k/475k)
      .conn_cap = envu("CURSED_CONN_CAP", 0),
      .reserve_mb = envu("CURSED_RESERVE_MB", 0),
      // shed band (permille of the cap) and the cap's hard ceiling; a fixed CURSED_CONN_CAP is clamped by the ceiling too
      // shedding law: headroom (default: f from memory headroom alone, no cap ceiling; explore/conn-policy/hband) or band
      .shed_headroom = !getenv("CURSED_SHED_MODE") || !*getenv("CURSED_SHED_MODE") || !strcmp(getenv("CURSED_SHED_MODE"), "headroom"),
      .shed_hi_mb = envu("CURSED_SHED_HI_MB", 400),
      .shed_lo_mb = envu("CURSED_SHED_LO_MB", 200),
      .shed_fmax = permille(envu("CURSED_SHED_FMAX", 500)),
      // no shedding by default (like Lean #153, which holds 280k on the box with none): every error the valve and floors
      // ever scored was one they sent, and at 285k they turned a survivable hold into 503s plus a 15 s connect tail.
      // Each knob below turns its part back on.
      .sock503_mbps = envu("CURSED_SOCK503_MBPS", 0) ? envu("CURSED_SOCK503_MBPS", 0) : (unsigned)-1,
      .sock503_avail_mb = envu("CURSED_SOCK503_AVAIL_MB", 450),
      .press_pending = envu("CURSED_PRESS_PENDING", 0) ? envu("CURSED_PRESS_PENDING", 0) : (unsigned)-1,   // 0: off
      .press_age_ms = envu("CURSED_PRESS_AGE_MS", 300),
      .cap_ceiling = envu("CURSED_CAP_CEILING", 380000),
      .policy_engage = permille(envu("CURSED_SHED_ENGAGE", 950)),
      .policy_release = permille(envu("CURSED_SHED_RELEASE", 880)),
      // CPU guard (permille of busy): freeze the close fraction above FREEZE, relax below RELAX; 1000+ disables it
      .guard_freeze = permille(envu("CURSED_GUARD_FREEZE", 920)),
      .guard_relax = permille(envu("CURSED_GUARD_RELAX", 850)),
      .busy_source = (int)envu("CURSED_BUSY_SRC", 0),
      // drain_first (arena drain tag, off by default): copy recv data out of the kernel at once and drain the
      // rings between batch chunks, so a lagging loop holds ~0.5 KB per pending request instead of a ~4 KB skb
      .drain_first = (int)envu("CURSED_DRAIN", 1),   // conn-policy combined tag: drain-first on by default
      .drain_chunk = (unsigned)envu("CURSED_DRAIN_CHUNK", 0),
      .drain_budget = (unsigned)envu("CURSED_DRAIN_BUDGET", 0),
      .rx_timestamps = (int)envu("CURSED_RX_TS", (unsigned)deadline_on),
      // recv_mode: LOOP_RECV_AUTO (0) = multishot recv. Poll mode (2) holds less kernel memory per idle connection but
      // keeps every FIN skb charged until its poll CQE is reaped, so a mass close at 300k costs +631 MB (measured).
      .recv_mode = (int)envu("CURSED_RECV_MODE", LOOP_RECV_AUTO),
      // memory floors (CURSED_MEM_ACCEPT_MB=200 CURSED_MEM_SHED_MB=120 restore the old ones): under 200 MB available
      // stop arming accepts (excess SYNs wait as connect time); under 120 MB FIN-close keep-alives idle >= 20 s, 2% per tick. Both with 32 MB hysteresis, on MemAvailable - SReclaimable.
      // At 100/60 the kernel's per-connection memory outran them at 500k (OOM); at 200/120 accepts park at ~490k and
      // 500k passed 2 of 2 in the arena, with no refusals at 475k. The alternative is the OOM killer.
      // Off by default since 1ecaac1's re-score: at 285k on the box the accept floor's parked SYNs made the p99 15 s.
      .mem_shed_floor_mb = envu("CURSED_MEM_SHED_MB", 0),
      .mem_accept_floor_mb = envu("CURSED_MEM_ACCEPT_MB", 0),
      .shed_fin = (int)envu("CURSED_SHED_FIN", 1),
      // tcp_mem close hint, off like the rest of the shedding: forced reconnects near the wall cost orphaned sockets
      // and TCP memory (ada #216 measured it at 280k). It never fired in the arena, where tcp_mem isn't readable
      // from a container's network namespace, but it was live on the box.
      .tcp_mem_close = (int)envu("CURSED_TCPMEM_CLOSE", 0),
      .on_batch = on_batch,
      .on_tick = on_tick,
  };
  if (cfg.shed_headroom) g_shed_mode = "headroom";
  stats_every = (int)envu("CURSED_STATS", 0);
  gL = loop_new(&cfg);
  if (!gL) {
    perror("server: loop_new");
    return 1;
  }
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  size_t locked = envu("CURSED_MLOCK", 1) ? lock_code() : 0;
  fprintf(stderr, "server: %s on %s:%d, parsers %s, %s, sqlite %s, code locked %zu KiB\n", loop_backend(gL), cfg.host, cfg.port,
          app_parsers(), app_db_mode(), sqlite3_libversion(), locked >> 10);
  int r = loop_run(gL);
  loop_free(gL);
  app_close();
  return r ? 1 : 0;
}
