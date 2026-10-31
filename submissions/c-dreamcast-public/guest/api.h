#pragma once

typedef struct {
    char *p;
    int len, cap, oom;
} buf_t;

enum { ROUTE_HEALTH, ROUTE_FEED, ROUTE_POST, ROUTE_CREATE, ROUTE_LIKE, ROUTE_OTHER, ROUTE_COUNT };

int api_init(char *err, int errlen);
/* Writes the JSON body to out and returns the HTTP status; *route says which endpoint it was. */
int api_handle(const char *method, int mlen, const char *path, int plen, const char *authz, int alen,
               const char *body, int blen, buf_t *out, int *route);
