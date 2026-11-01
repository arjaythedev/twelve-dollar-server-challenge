#ifndef ASTRAEA_SQLITE_ENGINE_H
#define ASTRAEA_SQLITE_ENGINE_H

#include <stddef.h>
#include <stdint.h>

typedef struct astraea_eng astraea_eng;

astraea_eng *astraea_eng_open(const char *path, int cache_kib);
void astraea_eng_close(astraea_eng *e);

int astraea_eng_writes_paused(astraea_eng *e);
int astraea_eng_begin_batch(astraea_eng *e);
int astraea_eng_commit_batch(astraea_eng *e);
void astraea_eng_after_batch(astraea_eng *e);

int astraea_eng_feed(astraea_eng *e, char *out, size_t cap, size_t *out_len);
int astraea_eng_get_post(astraea_eng *e, int64_t id, char *out, size_t cap, size_t *out_len);
int astraea_eng_like(astraea_eng *e, int64_t uid, int64_t pid, char *out, size_t cap, size_t *out_len);
int astraea_eng_create(astraea_eng *e, int64_t uid, const char *user, size_t user_len,
                       const char *body, size_t body_len, char *out, size_t cap, size_t *out_len);
int astraea_eng_health(astraea_eng *e, int64_t uptime_s, char *out, size_t cap, size_t *out_len);

int astraea_make_train_db(const char *path, int users, int posts, int likes);
void astraea_gcov_flush(void);

#endif
