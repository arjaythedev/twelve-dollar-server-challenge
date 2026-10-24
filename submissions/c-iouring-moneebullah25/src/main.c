/* Entry point: configuration, SQLite, then the event loop. */
#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "config.h"
#include "db.h"
#include "net.h"
#include "sqlite3.h"
#include "util.h"

int main(int argc, char **argv) {
    struct config cfg;
    config_defaults(&cfg);
    if (config_parse(&cfg, argc, argv)) return 2;
    if (cfg.max_head + cfg.max_body > 65000) {
        fprintf(stderr, "max-head + max-body must stay below 65000\n");
        return 2;
    }
    config_log(&cfg);

    /* THREADSAFE=2: two connections on two threads, each used by one thread. */
    if (sqlite3_config(SQLITE_CONFIG_MULTITHREAD) != SQLITE_OK || sqlite3_initialize() != SQLITE_OK) {
        log_msg("sqlite3_initialize failed");
        return 1;
    }
    log_msg("sqlite %s", sqlite3_libversion());
    char err[512];
    struct db *db = db_open(&cfg, err, sizeof err);
    if (!db) {
        log_msg("database: %s", err);
        return 1;
    }
    if (db_checkpointer_start(db)) {
        db_close(db);
        return 1;
    }
    if (!cfg.no_prefault) {
        int64_t t0 = now_ns();
        db_warm(db);
        log_msg("cache warm-up took %.0f ms", (double)(now_ns() - t0) / 1e6);
    }
    struct app *app = app_new(&cfg, db);
    if (!app) {
        log_msg("out of memory");
        db_close(db);
        return 1;
    }
    int rc = net_run(&cfg, app, db);
    app_free(app);
    db_close(db);
    sqlite3_shutdown();
    return rc ? 1 : 0;
}
