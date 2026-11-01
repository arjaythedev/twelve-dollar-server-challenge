#ifndef RAW_SQLITE_H
#define RAW_SQLITE_H

#include "sqlite_format.h"
#include <stdbool.h>

extern u32 root_users;
extern u32 root_posts;
extern u32 root_likes;
extern u32 root_likes_pk;
extern u32 root_posts_time;
extern u32 root_posts_user;
extern u32 root_likes_pid;

#define ROOT_USERS      root_users
#define ROOT_POSTS      root_posts
#define ROOT_LIKES      root_likes
#define ROOT_LIKES_PK   root_likes_pk
#define ROOT_POSTS_TIME root_posts_time
#define ROOT_POSTS_USER root_posts_user
#define ROOT_LIKES_PID  root_likes_pid

typedef struct {
  i64 id;
  i64 user_id;
  const u8 *body;
  u32 body_len;
  const u8 *created_at;
  u32 created_at_len;
  const u8 *author;
  u32 author_len;
  i64 like_count;
} raw_post_view_t;

typedef struct {
  u32 page_num;
  int cell_idx;
} raw_cursor_frame_t;

typedef struct {
  raw_cursor_frame_t stack[8];
  int depth;
  const u8 *page;
  u32 page_num;
  int cell_idx;
  int ncells;
  i64 rowid;
  const u8 *payload;
  u32 payload_len;
} raw_table_cursor_t;

typedef struct {
  raw_cursor_frame_t stack[8];
  int depth;
  const u8 *page;
  u32 page_num;
  int cell_idx;
  int ncells;
  int on_interior;
  const u8 *payload;
  u32 payload_len;
} raw_index_cursor_t;

int  raw_sqlite_open(const char *path);
void raw_sqlite_close(void);

void raw_sqlite_batch_begin(void);
int  raw_sqlite_batch_end(void);

int raw_seek_table(raw_table_cursor_t *cur, u32 root_page, i64 target_rowid);
int raw_last_table(raw_table_cursor_t *cur, u32 root_page);
int raw_prev_table(raw_table_cursor_t *cur);

int raw_index_seek_ge(raw_index_cursor_t *cur, u32 root_page, i64 target_post_id);
int raw_index_next(raw_index_cursor_t *cur);

int raw_get_username(i64 uid, const u8 **name, u32 *name_len);
int raw_post_exists(i64 post_id);
int raw_range_counts(i64 lo, i64 hi, i64 *counts);

int raw_insert_post(i64 uid, const u8 *body, u32 body_len, const u8 *created_at, u32 created_at_len,
                    const u8 *author, u32 author_len, raw_post_view_t *out_post);
int raw_insert_like(i64 uid, i64 post_id, const u8 *created_at, u32 created_at_len, bool *already_liked);

#endif
