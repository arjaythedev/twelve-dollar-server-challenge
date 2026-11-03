#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

/** Serves on @p host:@p port with @p threads event loops (0: one per core). Does not return
 *  unless listening fails. */
int server_run(const char *host, uint16_t port, const char *db_path, int threads);

#endif
