#ifndef CHALLENGE_H
#define CHALLENGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

/* Buffers retain capacity on reset. All append operations return 0 on success. */
int buf_reserve(Buf *buf, size_t extra);
int buf_append(Buf *buf, const void *data, size_t len);
int buf_json_string(Buf *buf, const char *text, size_t len);
int buf_int(Buf *buf, int64_t value);
int buf_post_row(Buf *buf, int64_t id, const char *body, size_t body_len,
                 const char *timestamp, size_t time_len, const char *name, size_t name_len,
                 int64_t likes);

int db_open(const char *path);
void db_close(void);
void db_begin_pass(void);
int db_writes_deferred(void);
int db_pass_read_only(void);
int db_commit_pass(void);
int db_feed(Buf *body);
int db_post(int64_t id, Buf *body);
int db_create(int64_t uid, const char *text, size_t text_len,
              const char *author, size_t author_len, Buf *body);
int db_like(int64_t uid, int64_t id);
int db_health(void);
const char *db_error(void);

int app_init(const char *secret);
/* Return 1 for a complete request, 0 for a fragment, or -1 on fatal input.
 * out starts empty. A response that can see a write must wait for commit. */
int app_request(const char *wire, size_t n, size_t *consumed, Buf *out, int *close_after);
void app_error_500(Buf *out);
int reactor_run(const char *host, const char *port);

#endif
