#ifndef TD_FEED_H
#define TD_FEED_H
struct sqlite3;
struct td_feed_row { long long id, likes; unsigned char *body, *created, *user; int body_len, user_len; };
struct td_feed { struct td_feed_row row[20]; unsigned char arena[65536]; };
typedef struct td_feed_row td_feed_row;
typedef struct td_feed td_feed;
int td_feed_init(struct sqlite3 *db);
int td_feed_read(int span);
int td_post_read(long long id);
const struct td_feed *td_feed_out(void);
#endif
