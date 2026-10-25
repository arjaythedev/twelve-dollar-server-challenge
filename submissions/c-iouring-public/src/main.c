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
              "commits_failed=%llu\n",
              (unsigned long long)es.checkpoints, (unsigned long long)es.ckpt_frames, es.ckpt_max_ms, es.ckpt_total_ms,
              (unsigned long long)es.wal_max, (unsigned long long)es.drains, (unsigned long long)es.drains_timed_out,
              es.drain_ms_max, (unsigned long long)es.busy_sleeps, (unsigned long long)es.restarts,
              (unsigned long long)es.wal_resets, (unsigned long long)(es.wal_dropped_bytes >> 20),
              (unsigned long long)es.wal_flushes, (unsigned long long)es.commits_failed);
      fprintf(stderr,
              "stats shed=%s open=%u acc=%llu req=%llu batches=%llu lanes=%llu slow=%llu slow_checked=%llu jwt=%llu held=%llu "
              "cfail=%llu deadline_503=%llu press_503=%llu sock_pressure=%u/%llu "
              "hint_closes=%llu close_frac=%.2f cap=%u busy=%.2f avail=%uMB orphans=%u listen_overflows=%llu "
              "listen_drops=%llu "
              "backlog=%u busy_src=%d utime=%ld.%06ld stime=%ld.%06ld maxrss=%ldKB\n",
              g_shed_mode, s->open, (unsigned long long)s->accepted, (unsigned long long)a->requests, (unsigned long long)a->batches,
              (unsigned long long)a->lanes, (unsigned long long)a->slow, (unsigned long long)a->slow_checked,
              (unsigned long long)a->jwts, (unsigned long long)a->held, (unsigned long long)a->commit_failures,
              (unsigned long long)a->deadline_503, (unsigned long long)a->press_503, s->sock_pressure, (unsigned long long)s->sock_pressure_events, (unsigned long long)s->hint_closes, s->close_frac, s->cap, s->busy,
              s->avail_mb, s->tcp_orphans, (unsigned long long)s->listen_overflows, (unsigned long long)s->listen_drops,
              s->backlog_now, s->busy_src, (long)ru.ru_utime.tv_sec, (long)ru.ru_utime.tv_usec,
              (long)ru.ru_stime.tv_sec, (long)ru.ru_stime.tv_usec, ru.ru_maxrss);
    }
  }
}

static unsigned envu(const char *k, unsigned d) {
  const char *v = getenv(k);
  return v && *v ? (unsigned)strtoul(v, 0, 10) : d;
}
static float permille(unsigned v) { return (float)v / 1000.0f; }

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
      .backlog = (int)envu("CURSED_BACKLOG",
                           4096),  // absorbs 100 ms loop stalls at ~14k accepts/s (measured on a 1-vCPU Xeon)
      .defer_accept = (int)envu("CURSED_DEFER_ACCEPT", 0),
      .linger_close = (int)strtol(getenv("CURSED_LINGER") ? getenv("CURSED_LINGER") : "0", 0, 10),
      .batch = envu("CURSED_BATCH", 128),
      .wait_us = envu("CURSED_WAIT_US", 2000),
      .conn_policy = (int)envu("CURSED_CONN_POLICY", 1),
      .conn_cap = envu("CURSED_CONN_CAP", 0),
      .reserve_mb = envu("CURSED_RESERVE_MB", 0),
      // shed band (permille of the cap) and the cap's hard ceiling; a fixed CURSED_CONN_CAP is clamped by the ceiling too
      // shedding law: headroom (default: f from memory headroom alone, no cap ceiling; explore/conn-policy/hband) or band
      .shed_headroom = !getenv("CURSED_SHED_MODE") || !*getenv("CURSED_SHED_MODE") || !strcmp(getenv("CURSED_SHED_MODE"), "headroom"),
      .shed_hi_mb = envu("CURSED_SHED_HI_MB", 400),
      .shed_lo_mb = envu("CURSED_SHED_LO_MB", 200),
      .shed_fmax = permille(envu("CURSED_SHED_FMAX", 500)),
      .sock503_mbps = envu("CURSED_SOCK503_MBPS", 50) ? envu("CURSED_SOCK503_MBPS", 50) : (unsigned)-1,
      .sock503_avail_mb = envu("CURSED_SOCK503_AVAIL_MB", 450),
      .cap_ceiling = envu("CURSED_CAP_CEILING", 380000),
      .policy_engage = permille(envu("CURSED_SHED_ENGAGE", 950)),
      .policy_release = permille(envu("CURSED_SHED_RELEASE", 880)),
      // CPU guard (permille of busy): freeze the close fraction above FREEZE, relax below RELAX; 1000+ disables it
      .guard_freeze = permille(envu("CURSED_GUARD_FREEZE", 920)),
      .guard_relax = permille(envu("CURSED_GUARD_RELAX", 850)),
      .busy_source = (int)envu("CURSED_BUSY_SRC", 0),
      .rx_timestamps = (int)envu("CURSED_RX_TS", (unsigned)deadline_on),
      // recv_mode: LOOP_RECV_AUTO (0) = multishot recv. Poll mode (2) holds less kernel memory per idle connection but
      // keeps every FIN skb charged until its poll CQE is reaped, so a mass close at 300k costs +631 MB (measured).
      .recv_mode = (int)envu("CURSED_RECV_MODE", LOOP_RECV_AUTO),
      // emergency floors below the cap policy: under 100 MB available stop arming accepts (excess SYNs
      // wait as connect time); under 60 MB FIN-close keep-alives idle >= 20 s, 2% per tick. Both with 32 MB hysteresis,
      // on MemAvailable - SReclaimable. They shouldn't trigger in a passing run; the alternative is the OOM killer.
      .mem_shed_floor_mb = envu("CURSED_MEM_SHED_MB", 60),
      .mem_accept_floor_mb = envu("CURSED_MEM_ACCEPT_MB", 100),
      .shed_fin = (int)envu("CURSED_SHED_FIN", 1),
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
  fprintf(stderr, "server: %s on %s:%d, parsers %s, %s, sqlite %s\n", loop_backend(gL), cfg.host, cfg.port,
          app_parsers(), app_db_mode(), sqlite3_libversion());
  int r = loop_run(gL);
  loop_free(gL);
  app_close();
  return r ? 1 : 0;
}
