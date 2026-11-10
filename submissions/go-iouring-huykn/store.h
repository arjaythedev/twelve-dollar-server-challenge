#ifndef HUYKN_STORE_H
#define HUYKN_STORE_H

#include <stdint.h>

typedef struct hkn_store hkn_store;

// Strings are counted UTF-8 byte ranges, including any embedded NULs. The arena
// belongs to the handle and is overwritten by its next read or insert.
typedef struct { uint32_t at, size; } hkn_span;
typedef struct {
  int64_t key, count;
  hkn_span content, timestamp, username;
} hkn_record;
typedef struct {
  unsigned char bytes[65536];
  hkn_record items[20];
  uint32_t used;
  int length;
} hkn_result;

hkn_store *hkn_connect(const char *filename, char *error, int capacity);
void hkn_disconnect(hkn_store *store);
const hkn_result *hkn_read(hkn_store *store, int single, int64_t key);
const hkn_result *hkn_create(hkn_store *store, int64_t user,
                           const char *body, int bytes);
// 201 inserted, 200 already present, 404 missing post, -1 database failure.
int hkn_vote(hkn_store *store, int64_t user, int64_t post);
int hkn_finish(hkn_store *store);
int hkn_tick(hkn_store *store, int64_t now);
int hkn_probe(hkn_store *store);
const char *hkn_error(hkn_store *store);

#endif
