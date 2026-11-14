#ifndef TD_FEED_H
#define TD_FEED_H
struct sqlite3;
struct td_feed_row { long long id, likes; unsigned char *body, *created, *user; int body_len, user_len; };
typedef struct td_feed_row td_feed_row;
int td_feed_init(struct sqlite3 *db);
unsigned char *td_feed_read(int span, unsigned char *d, unsigned char *dend);
unsigned char *td_post_read(long long id, unsigned char *d, unsigned char *dend);
#endif
