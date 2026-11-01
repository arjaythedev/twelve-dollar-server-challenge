#ifndef CHALLENGE_API_H
#define CHALLENGE_API_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { const char *p; size_t n; } Slice;
typedef struct { char *p; size_t len, cap; bool failed; } Buffer;
typedef struct { Buffer b; int status; bool pending_commit; } Reply;
int api_init(const char *path, const char *secret);
void api_shutdown(void);
void api_handle(Slice method, Slice path, Slice authorization, Slice body, Reply *reply);
void api_error(Reply *reply, int status, const char *message);
int api_finish(Reply *reply, bool close_connection, size_t *offset, size_t *length);
int api_commit_batch(void);
void api_log_diagnostics(void);
void api_checkpoint_maintenance(void);
#endif
