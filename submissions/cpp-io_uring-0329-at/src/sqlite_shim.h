#pragma once
#ifdef __cplusplus
extern "C" {
#endif

struct sqlite3;

struct shim_post {
  const char *body;       int body_len;
  const char *created_at; int created_len;
  const char *author;     int author_len;
};

struct shim_feed_entry {
  long long id;
  const char *body;       int body_len;
  const char *created_at; int created_len;
  const char *author;     int author_len;
  long long like_count;
};

int shim_init(struct sqlite3 *db);
void shim_txn_begin(struct sqlite3 *db);
void shim_txn_end(void);
int shim_get_post(struct sqlite3 *db, long long id, struct shim_post *out);
int shim_feed(struct sqlite3 *db, struct shim_feed_entry *out, int max);

#ifdef __cplusplus
}
#endif