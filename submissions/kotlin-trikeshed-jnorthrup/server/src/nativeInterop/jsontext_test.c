/*
 * feed_json_text (cinterop/feedc.def) against a byte-wise reference of Out.escapeFrom's rules: quote and backslash get a
 * backslash, \b \f \n \r \t keep their letters, the other bytes below 0x20 become \u00XX (lower-case hex), every other
 * byte (UTF-8, DEL) is copied. The function is cut out of feedc.def, so the test runs the shipped source.
 *
 *   cd server/src/nativeInterop
 *   awk '/BEGIN feed_json_text/,/END feed_json_text/' cinterop/feedc.def > /tmp/feed_json_text.h
 *   cc -O2 -fsanitize=address -I/tmp -o /tmp/jsontext_test jsontext_test.c && /tmp/jsontext_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "feed_json_text.h"

static int expected(const unsigned char *s, int n, char *d) {
  static const char hex[] = "0123456789abcdef";
  int w = 0;
  for (int i = 0; i < n; i++) {
    unsigned c = s[i];
    if (c == '"') { d[w++] = '\\'; d[w++] = '"'; }
    else if (c == '\\') { d[w++] = '\\'; d[w++] = '\\'; }
    else if (c >= 0x20) d[w++] = (char)c;
    else if (c == 0x08) { d[w++] = '\\'; d[w++] = 'b'; }
    else if (c == 0x0C) { d[w++] = '\\'; d[w++] = 'f'; }
    else if (c == 0x0A) { d[w++] = '\\'; d[w++] = 'n'; }
    else if (c == 0x0D) { d[w++] = '\\'; d[w++] = 'r'; }
    else if (c == 0x09) { d[w++] = '\\'; d[w++] = 't'; }
    else { d[w++] = '\\'; d[w++] = 'u'; d[w++] = '0'; d[w++] = '0'; d[w++] = hex[c >> 4]; d[w++] = hex[c & 15]; }
  }
  return w;
}

static unsigned long long cases;

static void fail(const unsigned char *s, int n, const char *want, int wn, const char *got, int gn, const char *why) {
  printf("FAIL (%s) after %llu cases, n=%d\n input:", why, cases, n);
  for (int i = 0; i < n && i < 64; i++) printf(" %02x", s[i]);
  printf("\n want(%d): %.*s\n got (%d): %.*s\n", wn, wn, want, gn, gn, got);
  exit(1);
}

static void check(const unsigned char *s, int n) {
  static char want[4096], got[4096];
  int wn = expected(s, n, want);
  memset(got, 0xEE, sizeof got);
  /* An exact-size heap copy, so a read past the end of the input aborts under -fsanitize=address. */
  unsigned char *exact = malloc(n ? (size_t)n : 1);
  memcpy(exact, s, (size_t)n);
  int gn = feed_json_text(exact, n, got);
  free(exact);
  cases++;
  if (gn != wn) fail(s, n, want, wn, got, gn < 0 ? 0 : gn, "length");
  if (memcmp(got, want, (size_t)wn) != 0) fail(s, n, want, wn, got, gn, "bytes");
  for (int i = wn; i < wn + 16; i++) if ((unsigned char)got[i] != 0xEE) fail(s, n, want, wn, got, gn, "wrote past the returned length");
}

static unsigned long long rng = 0x9E3779B97F4A7C15ULL;
static unsigned next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 16); }

int main(void) {
  static const unsigned char fillers[] = { 'a', 0xC3, 0x7F, 0x20, 0x21, 0x23, 0x1F, 0x5B, 0x5D, 0xA2, 0xDC, 0x80, 0xFF };
  unsigned char s[512];
  /* Every byte value at every position of every short length, against fillers next to the escaped values. */
  for (int v = 0; v < 256; v++)
    for (int len = 1; len <= 40; len++)
      for (int pos = 0; pos < len; pos++)
        for (unsigned f = 0; f < sizeof fillers; f++) {
          memset(s, fillers[f], (size_t)len);
          s[pos] = (unsigned char)v;
          check(s, len);
        }
  /* Empty and spot values. */
#define SPOT(lit) check((const unsigned char *)(lit), (int)sizeof(lit) - 1)
  SPOT("");
  SPOT("plain text, no escapes at all, longer than one eight-byte step");
  SPOT("say \"hi\" \\ there\n\t\r\b\f\x01\x1f caf\xC3\xA9 \xE2\x9C\x93");
  SPOT("\"");
  SPOT("\\\\\\\\\\\\\\\\\\");
  /* Random strings up to 300 bytes: uniform bytes, escape-heavy bytes and plain letters mixed. */
  static const unsigned char heavy[] = { 0x00, 0x08, 0x09, 0x0A, 0x0C, 0x0D, 0x1F, 0x22, 0x5C, 0x7F };
  for (int round = 0; round < 3000000; round++) {
    int len = (int)(next() % 301);
    unsigned kind = next() % 4;
    for (int i = 0; i < len; i++) {
      unsigned r = next();
      s[i] = kind == 0 ? (unsigned char)r
           : kind == 1 ? ((r & 3) == 0 ? heavy[(r >> 2) % sizeof heavy] : (unsigned char)('a' + (r >> 8) % 26))
           : kind == 2 ? ((r & 31) == 0 ? heavy[(r >> 5) % sizeof heavy] : (unsigned char)('a' + (r >> 8) % 26))
           : (unsigned char)('a' + (r >> 8) % 26);
    }
    check(s, len);
  }
  printf("jsontext ok: %llu cases\n", cases);
  return 0;
}
