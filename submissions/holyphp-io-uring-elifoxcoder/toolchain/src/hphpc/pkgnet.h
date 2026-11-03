/* pkgnet.h — minimal HTTP client for the HolyPHP package registry.
 *
 * Plain sockets (winsock2 on Windows, POSIX elsewhere), no TLS: registry URLs
 * are http://host:port[/base]. Used by `hphp install -r`, `hphp search -r`
 * and `hphp publish`.
 */
#ifndef HPHPC_PKGNET_H
#define HPHPC_PKGNET_H

#include <stddef.h>

/* Perform HTTP GET; returns malloc'd body (NUL-terminated, len out) or NULL.
 * URL forms: http://host:port/path — path defaults to "/". */
char *pkgnet_get(const char *url, size_t *out_len, int *status);

/* Perform HTTP POST of `body` (length len). Returns malloc'd response body
 * or NULL on network failure. */
char *pkgnet_post(const char *url, const char *content_type,
                  const char *body, size_t len, int *status);

#endif
