#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef long long i64;
typedef uint8_t u8;
typedef uint32_t u32;

int td_open(const char *path, char *err, int n);
void td_close(void);
int td_begin(void);
int td_commit(void);
void *td_cursor(int i);
int td_seek(void *c, i64 id);
int td_first(void *c);
int td_next(void *c);
const u8 *td_payload(void *c, u32 *len);
int td_count_likes(void *c, i64 lo, i64 hi, u32 *n);
int td_insert_post(i64 uid, const char *body, int len, i64 *id, char *created, int *clen);
int td_insert_like(i64 uid, i64 pid);
int td_delete_like(i64 uid, i64 pid);
int td_health(char *err, int n);
int td_feed_sql_step(i64 *id, const u8 **body, int *blen, const u8 **ca, int *clen, const u8 **author, int *alen, i64 *likes);
void td_feed_sql_reset(void);

int main(int argc, char **argv) {
  if (argc < 2) return 1;
  char err[256];
  if (td_open(argv[1], err, sizeof(err)) != 0) {
    fprintf(stderr, "td_open failed: %s\n", err);
    return 1;
  }

  for (int iter = 0; iter < 1000; iter++) {
    td_health(err, sizeof(err));

    td_feed_sql_reset();
    i64 id, likes;
    const u8 *b, *ca, *author;
    int blen, clen, alen;
    while (td_feed_sql_step(&id, &b, &blen, &ca, &clen, &author, &alen, &likes) == 1) {}
    td_feed_sql_reset();

    if (td_begin() == 0) {
      void *cp = td_cursor(0); // POSTS
      void *cu = td_cursor(1); // USERS
      void *ci = td_cursor(2); // FEED_IDX
      void *cl = td_cursor(3); // LIKES_IDX

      if (ci && td_first(ci) == 1) {
        for (int k = 0; k < 20; k++) {
          if (td_next(ci) != 1) break;
        }
      }
      if (cp) {
        td_seek(cp, 1);
        u32 len = 0;
        td_payload(cp, &len);
      }
      if (cu) {
        td_seek(cu, 1);
        u32 len = 0;
        td_payload(cu, &len);
      }
      if (cl) {
        u32 count = 0;
        td_count_likes(cl, 1, 1, &count);
      }

      i64 new_id = 0;
      char cr[64];
      int cr_len = 0;
      td_insert_post(1, "pgo training body", 17, &new_id, cr, &cr_len);
      if (new_id > 0) {
        td_insert_like(1, new_id);
        td_delete_like(1, new_id);
      }
      td_commit();
    }
  }

  td_close();
  return 0;
}
