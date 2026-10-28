/*
 * session: turns bytes into requests, for either I/O backend.
 *
 * The backend delivers what a connection received (session_on_data) or that its peer closed
 * (session_on_eof), and calls session_resume once a response has gone out. Each entry point ends by
 * closing the connection if it asked to be closed and has nothing left to send.
 */
#ifndef SESSION_H
#define SESSION_H

#include "app.h"

/* `len` bytes arrived on connection `id`. They are processed or kept; the caller may reuse the memory. */
void session_on_data(app *a, int id, const char *data, size_t len);

/* The peer closed its side, or the connection failed. */
void session_on_eof(app *a, int id);

/* A response went out, or a queued request was answered: process the requests that arrived behind it. */
void session_resume(app *a, int id);

#endif
