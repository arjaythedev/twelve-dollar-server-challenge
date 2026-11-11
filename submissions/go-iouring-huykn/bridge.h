#ifndef HUYKN_BRIDGE_H
#define HUYKN_BRIDGE_H
#include "store.h"

// These tokens represent only C-allocated stores. No Go address is encoded.
// Ownership stays explicit: open once, close once, and never use after close.
// Passing an integer avoids inspecting the same C pointer on every cgo call.
typedef uintptr_t hkn_handle;

static inline hkn_handle hkn_open_handle(const char *path, char *error, int capacity) {
  return (hkn_handle)hkn_connect(path, error, capacity);
}
static inline void hkn_close_handle(hkn_handle handle) {
  hkn_disconnect((hkn_store *)handle);
}
static inline const hkn_result *hkn_read_handle(hkn_handle handle, int single, int64_t key) {
  return hkn_read((hkn_store *)handle, single, key);
}
static inline const hkn_result *hkn_create_handle(hkn_handle handle, int64_t user, const char *body, int bytes) {
  return hkn_create((hkn_store *)handle, user, body, bytes);
}
static inline int hkn_vote_handle(hkn_handle handle, int64_t user, int64_t post) {
  return hkn_vote((hkn_store *)handle, user, post);
}
static inline int hkn_finish_handle(hkn_handle handle) {
  return hkn_finish((hkn_store *)handle);
}
static inline int hkn_probe_handle(hkn_handle handle) {
  return hkn_probe((hkn_store *)handle);
}
static inline const char *hkn_error_handle(hkn_handle handle) {
  return hkn_error((hkn_store *)handle);
}
#endif
