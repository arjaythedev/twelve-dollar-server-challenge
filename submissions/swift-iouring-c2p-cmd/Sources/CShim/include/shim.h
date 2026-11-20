/* The C surface Swift sees: the SQLite B-tree shim (shim.c), the io_uring event loop (loop.c) and
 * small byte helpers (util.c). Nothing here includes sqlite3.h or liburing.h. */
#ifndef SHIM_H
#define SHIM_H
#include <stddef.h>

/* ---- shim.c: SQLite ---- */
int td_open(const char *path, char *err, int n);
int td_begin(void);
int td_commit(void);
void td_tick(void);
int td_writes_held(void);
void td_rollback(void);
void *td_cursor(int i);
int td_seek(void *c, long long id);
int td_seek_ge(void *c, long long k);
int td_first(void *c);
int td_next(void *c);
const unsigned char *td_payload(void *c, unsigned *len);
long long td_rowid(void *c);
int td_count_likes(void *c, long long lo, long long hi, unsigned *n);
int td_insert_post(long long user_id, const unsigned char *body, int len, long long *id,
                   unsigned char *created, int *created_len);
int td_insert_like(long long user_id, long long post_id);
int td_health(char *err, int n);
int td_feed_sql_step(long long *id, const unsigned char **body, int *blen, const unsigned char **ca,
                     int *clen, const unsigned char **author, int *alen, long long *likes);

/* ---- util.c ---- */
/* JSON-escapes src[0..n) into dst (room for 6 * n bytes); returns the bytes written. */
size_t td_esc(const unsigned char *src, size_t n, unsigned char *dst);
/* 1 if s[0..n) is well-formed UTF-8 (no overlongs, surrogates or code points past U+10FFFF). */
int td_utf8_valid(const unsigned char *s, size_t n);
/* HMAC-SHA256 with the key's padded blocks hashed once, in td_hmac_init. */
void td_hmac_init(const unsigned char *secret, size_t n);
void td_hmac(const unsigned char *msg, size_t n, unsigned char out[32]);

/* ---- loop.c: the event loop ---- */
typedef struct {
  unsigned char *p;
  unsigned len, cap;
} lp_ob;

/* Makes room for `need` more bytes after o->len (may move o->p). */
void lp_ob_grow(lp_ob *o, unsigned need);

/* Handles the request at the start of buf[0..len), appending its response to out.
 * Returns 1 and sets *used when a whole request was answered, 0 when it is incomplete,
 * -1 when it is malformed (an error response was written; the connection closes), and
 * 2 when it is a write that must wait out a checkpoint hold (nothing was written). */
typedef int (*lp_handler)(const unsigned char *buf, unsigned len, lp_ob *out, unsigned *used);

/* Listens on host:port and runs the loop; returns only on a setup error (message on stderr). */
int lp_run(const char *host, int port, lp_handler handler);

#endif
