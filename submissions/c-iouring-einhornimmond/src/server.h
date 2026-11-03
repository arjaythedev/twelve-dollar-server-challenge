#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

/** Serves on @p host:@p port from one io_uring loop on the calling thread. Does not return
 *  unless setting up fails. */
int server_run(const char *host, uint16_t port, const char *db_path);

#endif
