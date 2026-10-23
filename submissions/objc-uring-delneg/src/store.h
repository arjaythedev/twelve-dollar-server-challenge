// SQLite access: one connection (EXCLUSIVE locking, WAL, synchronous=NORMAL)
// shared by all workers under one mutex. A worker takes the lock once per
// completion batch and runs the whole batch in one transaction; responses are
// sent only after COMMIT returns.
#pragma once
#include "td.h"
#include <pthread.h>
#include "sqlite3.h"

@interface TDStore : TDObject {
@public
    pthread_mutex_t lock;
}
- (instancetype)initWithPath:(const char *)path TD_DIRECT;
- (bool)begin TD_DIRECT;
- (bool)commit TD_DIRECT;
- (void)rollback TD_DIRECT;
// Each writes a JSON body into b and returns the HTTP status.
- (int)health:(TDBuf *)b uptime:(int64_t)up TD_DIRECT;
- (int)feed:(TDBuf *)b TD_DIRECT;
- (int)post:(int64_t)pid into:(TDBuf *)b TD_DIRECT;
- (int)create:(int64_t)uid name:(const char *)name length:(uint32_t)nl
          body:(const char *)body length:(uint32_t)bl into:(TDBuf *)b TD_DIRECT;
- (int)like:(int64_t)uid post:(int64_t)pid into:(TDBuf *)b TD_DIRECT;
@end
