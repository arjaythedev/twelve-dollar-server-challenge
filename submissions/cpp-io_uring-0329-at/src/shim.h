#pragma once
#ifdef __cplusplus
extern "C" {
#endif

int td_open(const char* path, char* err, int n);
int td_begin(void);
int td_commit(void);
void td_tick(void);
int td_writes_held(void);
void* td_cursor(int i);
int td_seek(void* c, long long id);
int td_seek_ge(void* c, long long k);
int td_first(void* c);
int td_next(void* c);
const unsigned char* td_payload(void* c, unsigned int* len);
int td_count_likes(void* c, long long lo, long long hi, unsigned int* n);
int td_insert_post(long long user_id, const char* body, int len, long long* id, char* created,
                   int* created_len);
int td_insert_like(long long user_id, long long post_id);
int td_health(char* err, int n);
int td_feed_sql_step(long long* id, const unsigned char** body, int* blen,
                     const unsigned char** ca, int* clen, const unsigned char** author, int* alen,
                     long long* likes);

#ifdef __cplusplus
}
#endif
