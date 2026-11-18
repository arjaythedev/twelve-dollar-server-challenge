/* Standalone correctness vectors for both SHA256_ROLLING16 configurations.
 * Build & run with both -DSHA256_ROLLING16=0 and =1. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"

static int eqhex(const uint8_t *p, const char *expected, unsigned n) {
  static const char h[] = "0123456789abcdef";
  for (unsigned i = 0; i < n; ++i)
    if (h[p[i] >> 4] != expected[2*i] || h[p[i] & 15] != expected[2*i+1]) return 0;
  return 1;
}
static int hash_case(const uint8_t *buf, size_t n, const char *expected) {
  struct sha256 ctx;
  uint8_t actual[32];
  sha256_init(&ctx);
  sha256_update(&ctx, buf, n);
  sha256_final(&ctx, actual);
  return eqhex(actual, expected, 32);
}
int main(void) {
  static const char *EMPTY = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  static const char *ABC = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  static const char *MILLION = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
  static const char *RFC4231 = "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7";
  if (!hash_case((const uint8_t *)"", 0, EMPTY) ||
      !hash_case((const uint8_t *)"abc", 3, ABC)) {
    fputs("SHA-256 small vectors failed\n", stderr);
    return 1;
  }
  uint8_t buf[1000];
  memset(buf, 'a', sizeof buf);
  struct sha256 ctx;
  uint8_t actual[32];
  sha256_init(&ctx);
  for (unsigned i = 0; i < 1000; ++i) sha256_update(&ctx, buf, sizeof buf);
  sha256_final(&ctx, actual);
  if (!eqhex(actual, MILLION, 32)) {
    fputs("SHA-256 million-character vector failed\n", stderr);
    return 1;
  }
  uint8_t key[20];
  memset(key, 0x0b, sizeof key);
  struct hmac_key hk;
  hmac_key_init(&hk, key, sizeof key);
  hmac_sha256(&hk, (const uint8_t *)"Hi There", 8, actual);
  if (!eqhex(actual, RFC4231, 32)) {
    fputs("RFC 4231 HMAC-SHA256 vector failed\n", stderr);
    return 1;
  }
  puts("SHA-256 / HMAC vectors OK");
  return 0;
}
