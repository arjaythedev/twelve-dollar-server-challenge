/* The $12 server challenge on gradido-unicorn's fast-path stack -- SQLite, libsodium and arnm, built
 * with zig -- with an io_uring loop where the stack has h2o. See README.md for the design. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

#include "db.h"
#include "jwt.h"
#include "server.h"

int main(void)
{
    const char *host = getenv("HOST"), *port = getenv("PORT");
    const char *secret = getenv("JWT_SECRET"), *db_path = getenv("SQLITE_PATH");
    struct rlimit limit;

    if (db_path == NULL || secret == NULL) {
        fprintf(stderr, "SQLITE_PATH and JWT_SECRET are required\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    /* The ring's table of connections may be as large as the descriptor limit: take all the
     * service is allowed. */
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        limit.rlim_cur = limit.rlim_max;
        (void)setrlimit(RLIMIT_NOFILE, &limit);
    }

    jwt_init(secret);
    return server_run(host != NULL ? host : "0.0.0.0", (uint16_t)atoi(port != NULL ? port : "3000"), db_path);
}
