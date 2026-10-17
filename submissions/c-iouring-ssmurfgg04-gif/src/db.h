/* db.h - SQLite layer: statements, feed/post/like/create/health */
#pragma once
#include <stddef.h>
#include <stdint.h>

void db_open(const char *path);
void db_close(void);

/* Each worker thread opens its own read connection (WAL snapshot) before
 * serving requests; call once at thread start. Idempotent. */
void db_thread_init(void);

/* instrumentation hook, called by server.c per handled request */
void db_req_note(long long us);

void h_health(int slot);
void h_feed(int slot);
void h_post(int slot, int64_t id);
void h_like(int slot, int64_t uid, int64_t pid);
void h_create(int slot, int64_t uid, const char *uname, size_t ulen,
              const char *body, size_t blen);
