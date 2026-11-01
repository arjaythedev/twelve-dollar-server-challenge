// Read paths on SQLite's B-tree layer (td_btree.c, compiled with the amalgamation).
#pragma once
#include <stdint.h>
#include "sqlite3.h"

typedef struct {
    int64_t id, likes;
    const char *body, *ca, *au;     // valid only during the sink call
    uint32_t bodyLen, caLen, auLen;
} TDPostView;
typedef void (*TDPostSink)(void *ctx, const TDPostView *v);

int td_bt_init(sqlite3 *db);        // 1: fast paths on
// Both run inside the caller's transaction and return -1 when the caller must
// answer with SQL instead (anything already given to the sink is then void).
int td_bt_feed(TDPostSink sink, void *ctx);                 // rows given to sink, newest first
int td_bt_post(int64_t id, TDPostSink sink, void *ctx);     // 1 found, 0 no such post
