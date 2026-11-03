/* router: maps a parsed request to an endpoint and answers it, or queues it in the batch. */
#ifndef ROUTER_H
#define ROUTER_H

#include "app.h"
#include "http_parse.h"

/*
 * Handles one complete request from connection `id`. Either the response was sent when this
 * returns, or the request waits in the batch and the connection is marked CONN_AWAITING_BATCH.
 */
void route_request(app *a, int id, const http_request *request);

#endif
