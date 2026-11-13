#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct dbx dbx;

enum { DBX_POSTS = 0, DBX_USERS = 1, DBX_FEED_IDX = 2, DBX_LIKES_IDX = 3 };

dbx *dbx_open(const char *path, char *err, int n);
void dbx_close(dbx *h);

int dbx_begin(dbx *h);
int dbx_commit(dbx *h);
int dbx_health(dbx *h, char *err, int n);
int dbx_checkpoint(dbx *h);
int dbx_wal_frames(dbx *h);

int dbx_first(dbx *h, int c);
int dbx_seek(dbx *h, int c, long long rowid);
int dbx_seek_ge(dbx *h, int c, long long key);
int dbx_next(dbx *h, int c);
const unsigned char *dbx_record(dbx *h, int c, unsigned *len);

long long dbx_likes(dbx *h, long long post_id);
int dbx_likes_many(dbx *h, const long long *ids, int n, unsigned *counts);

int dbx_insert_post(dbx *h, long long uid, const char *body, int n, long long *id, char *created,
                    int *clen);
int dbx_insert_like(dbx *h, long long uid, long long pid);

#ifdef __cplusplus
}
#endif
