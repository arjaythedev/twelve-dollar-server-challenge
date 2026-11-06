/* Synchronous SQLite/crypto bridge and native-owned response arenas.
 * The imported Pavlyhalim headers are never edited. Goose arguments are borrowed
 * only during these functions; no pointer into Goose survives the call. */
#define _GNU_SOURCE
#include "sqlite3.c"
#ifdef __linux__
#include "sys.h"
#else
/* Portable application objects for local probes. The actual transport is Linux. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
static uint64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static void prefault_stack(size_t n) { (void)n; }
/* Linux's per-thread niceness has no equivalent in this application-only build. */
static int g300_probe_priority(int which, id_t who, int priority) {
  (void)which; (void)who; (void)priority; return 0;
}
#define SYS_gettid 0
#define syscall(number) ((void)(number), 0)
#define setpriority g300_probe_priority
#define pthread_setname_np(thread, name) pthread_setname_np(name)
#endif
#include "app.h"
#ifndef __linux__
#undef syscall
#undef setpriority
#undef pthread_setname_np
#endif
#include "goose.h"

_Static_assert(sizeof(Connection_g) == 16, "connection layout changed");
_Static_assert(sizeof(Handle_g) == 8, "queue handle layout changed");
_Static_assert(sizeof(Fragment_g) == 20, "fragment layout changed");
_Static_assert(sizeof(Reply_g_Error_p) + 1 == 11, "error record layout changed");
_Static_assert(sizeof(Reply_g_Data_p) + 1 == 22, "data record layout changed");

static struct app A;
#define ARENA_CAP (1u << 20)
#define ARENA_POOL 64
#define ARENA_PREWARM 8
struct arena {
  struct arena *next;
  uint32_t refs, used, cap, id;
  uint8_t data[];
};
static struct arena *pool, *cur;
static unsigned pool_n, n_batch_arenas;
static struct arena *batch_arenas[256];
static struct arena **arena_ids;
static uint32_t arena_ids_cap;

static void allocation_failed(void) { perror("goose-300000 allocation"); exit(1); }
static struct arena *arena_new(void) {
  uint32_t id = 1;
  while (id < arena_ids_cap && arena_ids[id]) id++;
  if (id >= arena_ids_cap) {
    uint32_t old = arena_ids_cap;
    arena_ids_cap = old ? old * 2 : 64;
    void *p = realloc(arena_ids, arena_ids_cap * sizeof *arena_ids);
    if (!p) allocation_failed();
    arena_ids = p;
    memset(arena_ids + old, 0, (arena_ids_cap - old) * sizeof *arena_ids);
  }
  struct arena *a = malloc(sizeof *a + ARENA_CAP);
  if (!a) allocation_failed();
  a->cap = ARENA_CAP;
  a->id = id;
  arena_ids[id] = a;
  return a;
}
static struct arena *arena_get(void) {
  struct arena *a = pool;
  if (a) { pool = a->next; pool_n--; }
  else a = arena_new();
  a->refs = 0;
  a->used = 0;
  return a;
}
static void arena_unref(struct arena *a) {
  if (!a->refs) abort();
  if (--a->refs) return;
  if (pool_n < ARENA_POOL) { a->next = pool; pool = a; pool_n++; }
  else {
    arena_ids[a->id] = NULL;
    free(a);
  }
}
static uint8_t *arena_reserve(unsigned n) {
  if (n > ARENA_CAP) abort();
  if (!cur || cur->used + n > cur->cap) {
    cur = arena_get();
    cur->refs = 1;
    if (n_batch_arenas == 256) { fprintf(stderr, "batch too large\n"); exit(1); }
    batch_arenas[n_batch_arenas++] = cur;
  }
  return cur->data + cur->used;
}
static void prewarm_batch_memory(void) {
  for (unsigned i = 0; i < ARENA_PREWARM; i++) {
    struct arena *a = arena_new();
    memset(a->data, 0, a->cap);
    a->next = pool; pool = a; pool_n++;
  }
  g300_prewarm();
}
void g300_native_release_batch(void) {
  for (unsigned i = 0; i < n_batch_arenas; i++) arena_unref(batch_arenas[i]);
  n_batch_arenas = 0;
  cur = NULL;
}

WireRequest_g g300_native_parse(sl_u8 input) {
  struct req r;
  WireRequest_g out = {0};
  if (input.len < 0 || input.len > UINT32_MAX) { out.status_g = -1; return out; }
  out.status_g = http_parse(input.data, (unsigned)input.len, &r);
  if (out.status_g != 1) return out;
  out.method_g = (uint32_t)r.method;
  out.path_start_g = (uint32_t)((const uint8_t *)r.path - input.data);
  out.path_len_g = r.path_len;
  out.auth_start_g = r.auth ? (uint32_t)((const uint8_t *)r.auth - input.data) : 0;
  out.auth_len_g = r.auth_len;
  out.body_start_g = (uint32_t)((const uint8_t *)r.body - input.data);
  out.body_len_g = r.body_len;
  out.total_g = r.total_len;
  out.keep_alive_g = r.keep_alive;
  return out;
}
Auth_g g300_native_auth(sl_u8 auth, sl_u8 name) {
  Auth_g out = {0};
  if (auth.len < 7 || memcmp(auth.data, "Bearer ", 7)) { out.error_g = 4; return out; }
  struct jwt_claims c;
  int rc = jwt_verify(&A.key, (const char *)auth.data + 7, (size_t)auth.len - 7, A.now, &c);
  if (rc != JWT_OK) { out.error_g = rc == JWT_INVALID ? 5 : 6; return out; }
  if (name.len) {
    if (name.len < c.username_len) { out.error_g = 6; return out; }
    memcpy(name.data, c.username, c.username_len);
    out.name_len_g = c.username_len;
  }
  out.uid_g = c.sub;
  return out;
}
Body_g g300_native_body(sl_u8 body) {
  Body_g out = {0};
  struct jwant w[1] = {{"body", 4, {0}}};
  int type = json_doc((const char *)body.data, (size_t)body.len, w, 1);
  if (type < 0) { out.error_g = 7; return out; }
  if (type != J_OBJECT || w[0].v.type != J_STRING) { out.error_g = 8; return out; }
  out.start_g = (uint32_t)((const uint8_t *)w[0].v.p - body.data);
  out.len_g = (uint32_t)w[0].v.n;
  out.escaped_g = memchr(w[0].v.p, '\\', w[0].v.n) != NULL;
  return out;
}
int64_t g300_native_unescape(sl_u8 raw, sl_u8 out) {
  return json_unescape((const char *)raw.data, (size_t)raw.len, out.data, (size_t)out.len);
}
static Output_g response_error(uint8_t code, uint8_t database) {
  Output_g out = {0}; out.error_g = code; out.database_g = database; return out;
}
Output_g g300_native_response(uint32_t kind, uint64_t id, uint64_t uid,
                              sl_u8 name, sl_u8 text, uint8_t close) {
  uint8_t *dst = arena_reserve(RESP_MAX);
  struct ob o;
  ob_init(&o, dst + HDR_ROOM, RESP_MAX - HDR_ROOM);
  const char *status = "200 OK";
  unsigned status_n = 6;
  int rc;
  uint8_t database = kind != 1;
  switch (kind) {
  case 1:
    if (db_health(&A.db) < 0) return response_error(12, 0);
    ob_lit(&o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    ob_u64(&o, (uint64_t)(A.now - A.start)); ob_char(&o, '}');
    break;
  case 2:
    if (db_feed(&A.db, &o) < 0) goto failed;
    break;
  case 3:
    rc = db_post(&A.db, &o, (int64_t)id);
    if (rc < 0) goto failed;
    if (!rc) return response_error(3, 1);
    break;
  case 4:
    rc = db_like(&A.db, (int64_t)uid, (int64_t)id);
    if (rc < 0) goto failed;
    if (!rc) return response_error(3, 1);
    ob_lit(&o, "{\"liked\":true,\"already_liked\":");
    if (rc == 2) ob_lit(&o, "true,\"post_id\":"); else ob_lit(&o, "false,\"post_id\":");
    ob_u64(&o, id); ob_char(&o, '}');
    if (rc == 1) { status = "201 Created"; status_n = 11; }
    break;
  case 5:
    rc = db_create(&A.db, &o, (int64_t)uid, text.data, (unsigned)text.len, name.data, (unsigned)name.len);
    /* db_create uses SQLITE_STATIC. reset alone retains that binding. */
    sqlite3_clear_bindings(A.db.st_ins_post);
    if (rc < 0) goto failed;
    status = "201 Created"; status_n = 11;
    break;
  default: goto failed;
  }
  if (o.err) goto failed;
  struct resp r;
  resp_frame(&r, dst, status, status_n, (unsigned)o.n, close);
  cur->used += (uint32_t)((r.p + r.n) - dst);
  return (Output_g){cur->id, (uint32_t)(r.p - cur->data), r.n, 0, database};
failed:
  return response_error(10, database);
}

static const uint8_t *static_response(uint8_t code, uint32_t *n) {
  switch (code) {
#define RESP_CASE(c, name) case c: *n = L_##name; return R_##name
    RESP_CASE(1, 404); RESP_CASE(2, 400_ID); RESP_CASE(3, 404_POST);
    RESP_CASE(4, 401_MISSING); RESP_CASE(5, 401_INVALID); RESP_CASE(6, 401_PAYLOAD);
    RESP_CASE(7, 400_JSON); RESP_CASE(8, 400_BODY); RESP_CASE(9, 400_LONG);
    RESP_CASE(10, 500); RESP_CASE(11, 400_BAD); RESP_CASE(12, 503);
#undef RESP_CASE
  default: abort();
  }
}
