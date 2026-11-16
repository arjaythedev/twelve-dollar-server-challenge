#ifndef SQLITE_LAYER_H
#define SQLITE_LAYER_H

#include <stddef.h>
#include <stdint.h>

int db_open(const char *path, char *err, size_t err_cap);
void db_close(void);

int db_health(char *err, size_t err_cap);

void db_tick(void);

int db_writes_held(void);

int db_begin(void);
int db_commit(void);
void db_rollback(void);

int db_read_feed(uint8_t *out, size_t out_cap, size_t *out_len);

int db_read_post(int64_t post_id, uint8_t *out, size_t out_cap, size_t *out_len);

int db_insert_post(int64_t user_id, const uint8_t *body, size_t body_len,
                   int64_t *id_out, char *created_out, size_t created_cap,
                   size_t *created_len_out);

int db_insert_like(int64_t user_id, int64_t post_id, int *already_liked_out);

#endif
