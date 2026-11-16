#ifndef URING_NET_H
#define URING_NET_H

#include <stddef.h>
#include <stdint.h>

struct net_ctx;

typedef void (*net_read_cb)(struct net_ctx *ctx, uint32_t conn_id, void *user);

struct net_ctx *net_init(const char *host, uint16_t port, int pre_conns);
void net_destroy(struct net_ctx *ctx);

int net_poll_pass(struct net_ctx *ctx, int wait_us, int min_cqes, int max_answers,
                  net_read_cb on_ready, void *user);

void net_conn_buf(struct net_ctx *ctx, uint32_t conn_id, const uint8_t **data, size_t *len);

void net_conn_consume(struct net_ctx *ctx, uint32_t conn_id, size_t n);

int net_conn_is_eof(struct net_ctx *ctx, uint32_t conn_id);

int net_conn_is_dead(struct net_ctx *ctx, uint32_t conn_id);

uint8_t *net_batch_alloc(struct net_ctx *ctx, size_t size);

int net_conn_send(struct net_ctx *ctx, uint32_t conn_id, const uint8_t *data, size_t len,
                  int is_static, int close_after);

void net_conn_close(struct net_ctx *ctx, uint32_t conn_id);

void net_conn_requeue(struct net_ctx *ctx, uint32_t conn_id);

void net_end_batch(struct net_ctx *ctx);

int net_has_backlog(struct net_ctx *ctx);

#endif
