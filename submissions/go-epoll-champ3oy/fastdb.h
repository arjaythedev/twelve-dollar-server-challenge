// The C side of the server: every SQLite call and every byte of post JSON happens here, so each
// request crosses from Go into C exactly once.
#ifndef FASTDB_H
#define FASTDB_H

#include <stdint.h>

// Return codes shared by the functions below (lengths are >= 0).
#define FDB_ERR      -1  // SQLite error
#define FDB_NOSPACE  -2  // the output buffer is too small

int fdb_open(const char *path, char *err, int errcap);
int fdb_ping(char *err, int errcap);

// Render {"posts":[...]} / {"post":{...}} into out. fdb_post returns 0 when the post doesn't exist.
int fdb_feed(char *out, int cap);
int fdb_post(int64_t id, char *out, int cap);

// Writes, run by the group-commit loop between fdb_begin and fdb_commit.
int fdb_begin(void);
int fdb_commit(void);
int fdb_rollback(void);
// 1 = inserted, 2 = already liked, 0 = no such post, FDB_ERR.
int fdb_like(int64_t user_id, int64_t post_id);
// Inserts the post and renders its {"post":{...}} response.
int fdb_create(int64_t user_id, const char *body, int blen, const char *author, int alen,
               char *out, int cap);

// WAL frames after the last commit (main connection).
int fdb_wal_frames(void);
// A second connection, used only by the checkpoint thread.
int fdb_ckpt_open(const char *path);
int fdb_checkpoint(void);

#endif
