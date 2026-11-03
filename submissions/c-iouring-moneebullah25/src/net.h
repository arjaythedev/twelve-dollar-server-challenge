/* The event loop: one thread, one io_uring per 65,535 connections (direct
 * descriptors are capped per ring by the hard RLIMIT_NOFILE), multishot
 * accept and receive, provided buffers, a bounded send-buffer pool, and one
 * batch of requests per wake-up handed to the app layer. */
#ifndef NET_H
#define NET_H

#include "app.h"
#include "config.h"

/* Runs until SIGINT/SIGTERM or exit_after_s. Returns 0 on a clean exit. */
int net_run(const struct config *cfg, struct app *app, struct db *db);

#endif
