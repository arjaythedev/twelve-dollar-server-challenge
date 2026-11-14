/* The Goose values that cross into net.c and db.c (Goose spec 7.10): a slice arrives as
 * { data, len }, a struct as its packed fields, a builder as goose.h's gs_rref. */
#ifndef FEED_FFI_H
#define FEED_FFI_H
#include <stdint.h>
#include "goose.h"

#pragma pack(push, 1)
typedef struct { const uint8_t *data; int64_t len; } bytes_t;
typedef struct { int64_t id, likes, body_len, created_len, author_len; } feed_row_t;
#pragma pack(pop)

#endif
