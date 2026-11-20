#include "challenge.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const char *path = getenv("SQLITE_PATH"), *secret = getenv("JWT_SECRET");
    const char *host = getenv("HOST"), *port = getenv("PORT");
    if (!path || !secret || !host || !port) {
        fputs("Set SQLITE_PATH, JWT_SECRET, HOST and PORT.\n", stderr);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    if (app_init(secret) || db_open(path)) return 1;
    int rc = reactor_run(host, port);
    db_close();
    return rc == 0 ? 0 : 1;
}
