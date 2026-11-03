/* server: start-up (signals, descriptor limit, listening sockets) and the choice of event loop. Single-threaded: one vCPU gains nothing from more threads. */
#ifndef SERVER_H
#define SERVER_H

#include "config.h"

/* Serves until SIGINT or SIGTERM, then checkpoints the database and returns. */
void server_run(const config *cfg);

#endif
