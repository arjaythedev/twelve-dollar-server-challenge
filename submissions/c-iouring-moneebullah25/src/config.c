#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void config_defaults(struct config *c) {
    memset(c, 0, sizeof *c);
    c->db_path = "feed.db";
    c->jwt_secret = "twelve-dollar-challenge";
    c->host = "0.0.0.0";
    c->port = 80;
    c->max_conns = 450000;
    c->ring_slots = 0;
    c->recv_bufs = 2048;
    c->recv_buf_size = 2048;
    c->out_chunks = 2048;
    c->out_chunk_size = 16384;
    c->batch_cqes = 32;
    c->batch_wait_us = 250;
    c->work_budget = 256;
    c->max_head = 8192;
    c->max_body = 16384;
    c->partial_timeout_s = 30;
    c->mem_stop_accept_mb = 192;
    c->mem_shed_mb = 96;
    c->sqlite_cache_kib = 32768;
    c->ckpt_frames = 4000;
    c->hold_frames = 8192;
    c->hold_timeout_ms = 500;
}

struct opt {
    const char *name;
    uint32_t *u32;
    int *flag;
    const char *help;
};

static void usage(const struct opt *opts, size_t n) {
    fprintf(stderr, "usage: server [options]\n"
                    "environment: SQLITE_PATH, JWT_SECRET, HOST, PORT (see SPEC.md)\n"
                    "options:\n");
    for (size_t i = 0; i < n; i++) {
        fprintf(stderr, "  --%-22s %s%s\n", opts[i].name, opts[i].help, opts[i].u32 ? " (number)" : "");
    }
}

int config_parse(struct config *c, int argc, char **argv) {
    const char *e;
    if ((e = getenv("SQLITE_PATH")) && *e) c->db_path = e;
    if ((e = getenv("JWT_SECRET")) && *e) c->jwt_secret = e;
    if ((e = getenv("HOST")) && *e) c->host = e;
    if ((e = getenv("PORT")) && *e) c->port = atoi(e);

    const struct opt opts[] = {
        {"max-conns", &c->max_conns, NULL, "connection table size"},
        {"ring-slots", &c->ring_slots, NULL, "fixed-file slots per ring (0 = from RLIMIT_NOFILE)"},
        {"recv-bufs", &c->recv_bufs, NULL, "receive buffers per ring"},
        {"recv-buf-size", &c->recv_buf_size, NULL, "bytes per receive buffer"},
        {"out-chunks", &c->out_chunks, NULL, "send buffers in the pool"},
        {"out-chunk-size", &c->out_chunk_size, NULL, "bytes per send buffer"},
        {"batch-cqes", &c->batch_cqes, NULL, "completions per batch"},
        {"batch-wait-us", &c->batch_wait_us, NULL, "batch wait in microseconds"},
        {"work-budget", &c->work_budget, NULL, "requests per loop iteration"},
        {"max-head", &c->max_head, NULL, "request header limit"},
        {"max-body", &c->max_body, NULL, "request body limit"},
        {"partial-timeout-s", &c->partial_timeout_s, NULL, "timeout for incomplete requests"},
        {"mem-stop-accept-mb", &c->mem_stop_accept_mb, NULL, "pause accepting below this MemAvailable"},
        {"mem-shed-mb", &c->mem_shed_mb, NULL, "close idle connections below this MemAvailable"},
        {"sqlite-cache-kib", &c->sqlite_cache_kib, NULL, "SQLite page cache"},
        {"ckpt-frames", &c->ckpt_frames, NULL, "WAL frames per passive checkpoint"},
        {"hold-frames", &c->hold_frames, NULL, "WAL frames that start a write hold"},
        {"hold-timeout-ms", &c->hold_timeout_ms, NULL, "maximum write hold"},
        {"stats-interval-s", &c->stats_interval_s, NULL, "print a stats line this often"},
        {"exit-after-s", &c->exit_after_s, NULL, "exit after this many seconds"},
        {"rlimit-nofile", &c->rlimit_nofile, NULL, "tests: lower RLIMIT_NOFILE (soft and hard)"},
        {"feed-reference", NULL, &c->feed_reference, "use the reference feed query"},
        {"no-batch-read-txn", NULL, &c->no_batch_read_txn, "one implicit transaction per read"},
        {"no-prefault", NULL, &c->no_prefault, "skip startup prefaulting"},
    };
    const size_t nopts = sizeof opts / sizeof opts[0];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--", 2) != 0) {
            usage(opts, nopts);
            return -1;
        }
        const char *eq = strchr(a, '=');
        size_t nlen = eq ? (size_t)(eq - a - 2) : strlen(a + 2);
        const struct opt *o = NULL;
        for (size_t j = 0; j < nopts; j++) {
            if (strlen(opts[j].name) == nlen && strncmp(opts[j].name, a + 2, nlen) == 0) o = &opts[j];
        }
        if (!o) {
            usage(opts, nopts);
            return -1;
        }
        if (o->flag) {
            *o->flag = 1;
            continue;
        }
        const char *val = eq ? eq + 1 : (i + 1 < argc ? argv[++i] : NULL);
        char *endp;
        if (!val || !*val) {
            usage(opts, nopts);
            return -1;
        }
        unsigned long v = strtoul(val, &endp, 10);
        if (*endp || v > UINT32_MAX) {
            usage(opts, nopts);
            return -1;
        }
        *o->u32 = (uint32_t)v;
    }
    if (c->port <= 0 || c->port > 65535 || c->max_conns == 0 || c->recv_bufs == 0 || c->out_chunks == 0 ||
        c->out_chunk_size < 4096 || c->work_budget == 0 || c->batch_cqes == 0 || c->recv_buf_size < 512) {
        fprintf(stderr, "invalid configuration\n");
        return -1;
    }
    return 0;
}

void config_log(const struct config *c) {
    log_msg("config: db=%s host=%s port=%d max_conns=%u ring_slots=%u recv_bufs=%u x %u out_chunks=%u x %u "
            "batch=%u/%uus budget=%u cache=%u KiB ckpt=%u hold=%u/%ums mem=%u/%u MB feed=%s read_txn=%s",
            c->db_path, c->host, c->port, c->max_conns, c->ring_slots, c->recv_bufs, c->recv_buf_size, c->out_chunks,
            c->out_chunk_size, c->batch_cqes, c->batch_wait_us, c->work_budget, c->sqlite_cache_kib, c->ckpt_frames,
            c->hold_frames, c->hold_timeout_ms, c->mem_stop_accept_mb, c->mem_shed_mb,
            c->feed_reference ? "reference" : "range-scan", c->no_batch_read_txn ? "off" : "on");
}
