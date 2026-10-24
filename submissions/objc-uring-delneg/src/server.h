#pragma once
#include "td.h"
#include "store.h"
#include <liburing.h>

enum { R_ERR = 0, R_HEALTH, R_FEED, R_POST, R_CREATE, R_LIKE, R_COUNT };

typedef struct {
    uint32_t slot, gen;
    uint8_t route, close;
    int16_t status;           // R_ERR
    const char *msg;          // R_ERR: static error message
    int64_t id, uid;
    uint32_t nameOff, nameLen, bodyOff, bodyLen; // into the worker arena
} TDReq;

@class TDWorker;

// One handler object per route. Workers look up each handler's IMP once at
// startup and call it directly (IMP caching), skipping objc_msgSend lookups.
@interface TDHandler : TDObject
- (int)handle:(TDReq *)r worker:(TDWorker *)w body:(TDBuf *)b;
@end

typedef struct {
    TDStore *store;
    TDAuth *auth;
    int64_t started;
    uint32_t slots;           // fixed-file slots per ring
    int listenFd;
    int rings;
} TDWorkerConfig;

@interface TDWorker : TDObject {
@public
    TDStore *store;
    TDBuf arena;              // per-batch strings (names, bodies)
    int64_t uptime;
}
- (instancetype)initWithConfig:(const TDWorkerConfig *)cfg TD_DIRECT;
- (void)run TD_DIRECT;
@end
