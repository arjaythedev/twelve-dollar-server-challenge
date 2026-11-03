#ifndef TD_CKPT_H
#define TD_CKPT_H

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* gc.h renames pthread_create so that the GC registers every new thread; this one must stay unknown to it. */
#undef pthread_create
#undef pthread_detach

/* The checkpoint thread is a plain pthread, not a V thread: the Boehm GC signals every thread it
   knows to stop the world, and a thread stuck in fsync on a stalled disk cannot answer, which makes
   the GC abort the process ("Signals delivery fails constantly"). This thread never touches GC memory. */
struct td_ckpt {
    char *path;
    int req, done;
};

static void *td_ckpt_main(void *arg) {
    struct td_ckpt *c = arg;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(c->path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
        abort();
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0", NULL, NULL, NULL);
    for (;;) {
        uint64_t v;
        if (read(c->req, &v, 8) != 8)
            continue;
        int log = 0, done = 0;
        int rc = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
        v = rc == SQLITE_OK && log == done ? 1 : 2;
        if (write(c->done, &v, 8) != 8)
            abort();
    }
    return NULL;
}

/* td_ckpt_start runs one checkpoint per 8-byte write to req and answers on done with 1 when it
   copied the whole WAL (the next write then restarts the WAL) or 2 when frames were appended meanwhile. */
static void td_ckpt_start(const char *path, int req, int done) {
    struct td_ckpt *c = malloc(sizeof *c);
    c->path = strdup(path);
    c->req = req;
    c->done = done;
    pthread_t t;
    if (pthread_create(&t, NULL, td_ckpt_main, c) != 0)
        abort();
    pthread_detach(t);
}

#endif
