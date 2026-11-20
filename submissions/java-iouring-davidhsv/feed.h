/* What libfeed.so exports to Java. Everything else in the library, SQLite included, is built
 * with hidden visibility. */
#ifndef FEED_H
#define FEED_H
#include <stdint.h>

#define API __attribute__((visibility("default")))

/* Results that do not fit a return value, read by Java at the address db_io() gives: a record's
 * length, a new post's id and created_at, a row of the reference feed query. The pointers stay
 * valid until the next call on the same cursor or statement. Field offsets are fixed: Java reads
 * them by offset. */
typedef struct {
  int64_t len;        /*  0: db_rec's record length */
  int64_t id;         /*  8: db_add_post's id, db_feed_step's post id */
  int64_t likes;      /* 16: db_feed_step's like count */
  uint64_t text[3];   /* 24: db_add_post's created_at in text[0]; db_feed_step's body, created_at, author */
  int64_t text_len[3]; /* 48 */
} feed_io_t;

#endif
