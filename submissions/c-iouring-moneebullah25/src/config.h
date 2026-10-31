/* Runtime configuration: the four SPEC environment variables plus tunables
 * with defaults sized for a 1 vCPU / 2 GB box. Everything here is a setting
 * inside our own process (rule 11); nothing touches the host. */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

struct config {
    const char *db_path;
    const char *jwt_secret;
    const char *host;
    int port;

    uint32_t max_conns;         /* connection table size across all rings */
    uint32_t ring_slots;        /* fixed-file slots per ring; 0 = min(hard RLIMIT_NOFILE, 65535) */
    uint32_t recv_bufs;         /* provided receive buffers per ring */
    uint32_t recv_buf_size;     /* bytes per receive buffer */
    uint32_t out_chunks;        /* send buffers in the shared pool */
    uint32_t out_chunk_size;    /* bytes per send buffer (holds the largest response) */
    uint32_t batch_cqes;        /* completions to wait for before a batch runs */
    uint32_t batch_wait_us;     /* ... or this long, whichever comes first */
    uint32_t work_budget;       /* requests handled per loop iteration */
    uint32_t max_head;          /* request line + headers limit */
    uint32_t max_body;          /* request body limit */
    uint32_t partial_timeout_s; /* close connections that leave a request incomplete this long */
    uint32_t shed_hi_mb;        /* available memory above which no response carries Connection: close */
    uint32_t shed_lo_mb;        /* ... at or below which every response does, and idle keep-alives are closed */
    uint32_t conn_cap;          /* open connections at which every response closes (shedding starts at 90%) */
    uint32_t accept_floor_mb;   /* available memory below which accepting pauses (last resort) */
    uint32_t idle_timeout_s;    /* close keep-alives that sent nothing for this long; 0 = never */
    double shed_force;          /* tests: fixed shed fraction 0..1 instead of the memory/connection laws */
    uint32_t sqlite_cache_kib;  /* PRAGMA cache_size, in KiB */
    uint32_t ckpt_frames;       /* WAL frames that trigger a passive checkpoint */
    uint32_t hold_frames;       /* WAL frames that trigger a write hold so the WAL can restart */
    uint32_t hold_timeout_ms;   /* give up a hold after this long */
    uint32_t stats_interval_s;  /* 0 = no periodic stats line */
    uint32_t exit_after_s;      /* 0 = run forever (used by the PGO training run) */
    uint32_t rlimit_nofile;     /* tests only: lower both RLIMIT_NOFILE limits to this */
    int feed_reference;         /* 1 = always use the reference feed SQL */
    int no_batch_read_txn;      /* 1 = no shared read transaction per batch */
    int no_prefault;            /* 1 = skip the SQLite cache warm-up at startup */
    int prefault;               /* 1 = touch every page of the pools and tables at startup (opt-in) */
};

void config_defaults(struct config *c);
/* Reads the environment and argv; returns 0, or -1 after printing usage. */
int config_parse(struct config *c, int argc, char **argv);
void config_log(const struct config *c);

#endif
