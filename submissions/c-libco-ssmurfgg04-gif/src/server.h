/* server.h - what the rest of the server offers the layers */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Queue a response on a connection's output buffer; body copied in.
 * close_conn: 0 keep-alive, 1 close after this response is sent. */
void respond(int slot, int status, const char *body, size_t n, int close_conn);

/* Reserve header + body space in the connection's output buffer and return
 * where the caller writes exactly n body bytes. */
char *resp_alloc(int slot, int status, size_t n, int close_conn);

/* Fixed error bodies ({"error":"..."}). */
void srv_err(int slot, int status, const char *msg);
void srv_err_500(int slot);
void srv_err_close(int slot, int status, const char *msg);

/* Expect: 100-continue + Connection header mode. */
int srv_sent_continue(int slot);
void srv_send_continue(int slot);
void srv_reset_continue(int slot);
void srv_set_conn_hdr(int slot, int mode);

/* Connection introspection used by the transaction rollback logic. */
int conn_open(int slot);
uint32_t conn_gen(int slot);
uint32_t conn_out_len(int slot);
void conn_truncate_out(int slot, uint32_t to);

/* Event-loop entry points. */
size_t handle_request(int slot, const char *p, size_t n);
void on_data(int slot, const char *data, size_t n);

#ifdef __cplusplus
}
#endif
