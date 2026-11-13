#define _GNU_SOURCE
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    struct bc_config cfg;
    if (bc_config_read(&cfg) < 0) return 1;

    struct bc_server s;
    if (bc_server_init(&s, &cfg) < 0) {
        fprintf(stderr, "failed to initialize server\n");
        return 1;
    }

    fprintf(stderr, "BinaryCore listening on %s:%s (SQLITE_PATH=%s, cache=%d KiB)\n",
            cfg.host, cfg.port, cfg.db_path, cfg.cache_kib);

    bc_server_run(&s);

    bc_server_cleanup(&s);
    return 0;
}
