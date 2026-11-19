#ifndef TWELVE_SQLITE_FAST_H
#define TWELVE_SQLITE_FAST_H

#include <stdint.h>

typedef struct {
  int64_t id;
  int64_t like_count;
  uint32_t body_len;
  uint32_t created_at_len;
  uint32_t author_len;
  uint8_t body[2048];
  uint8_t created_at[32];
  uint8_t author[256];
} TwelveFastRow;

void *twelve_fast_open(void *sqlite_db);
void twelve_fast_close(void *context);
int twelve_fast_batch_begin(void *context);
void twelve_fast_batch_end(void *context);
void twelve_fast_prepare_write(void *context);
int twelve_fast_read(void *context, int is_feed, int64_t post_id,
                     TwelveFastRow *rows, int capacity, int *count);
int twelve_fast_before_insert(void *context);
void twelve_fast_cancel_insert(void *context);
void twelve_fast_after_insert(void *context, int64_t id, int64_t user_id,
                              const uint8_t *created_at, uint32_t length);
void twelve_fast_rollback(void *context);

#endif
