/* http.h */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parses one request at the start of [p, p+n); routes it; returns bytes
 * consumed, 0 if the request is incomplete so far. */
size_t handle_request(int slot, const char *p, size_t n);
#ifdef __cplusplus
}
#endif
