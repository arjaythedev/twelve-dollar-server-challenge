// What the Go side sees of shim.c.
#include <stdint.h>

#define TD_ROWS 20
#define TD_TEXT 65536

typedef struct {
  long long id;
  long long likes;
  uint32_t body_off, body_len;
  uint32_t created_off, created_len;
  uint32_t author_off, author_len;
} td_row_t;

// The result of the last read: rows plus their text, copied out of SQLite's pages.
typedef struct {
  int32_t nrows;
  uint32_t ntext;
  td_row_t rows[TD_ROWS];
  unsigned char text[TD_TEXT];
} td_out_t;

extern td_out_t td_out;

int td_open(const char *path, char *err, int n);
int td_commit(void);
void td_tick(void);
int td_writes_held(void);
int td_feed(void);
int td_post(long long id);
int td_like(long long uid, long long pid);
int td_insert_post(long long uid, const char *body, int len);
int td_health(void);
