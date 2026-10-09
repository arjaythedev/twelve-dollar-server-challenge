/* Decode one Unicode scalar; reject truncated, overlong and surrogate encodings. */
static size_t utf8_step(const char *text, size_t left, uint32_t *code) {
  const unsigned char *s = (const unsigned char *)text;
  if (!left) return 0;
  unsigned n = s[0] < 0x80 ? 1 : s[0] >= 0xc2 && s[0] <= 0xdf ? 2 :
               s[0] >= 0xe0 && s[0] <= 0xef ? 3 : s[0] >= 0xf0 && s[0] <= 0xf4 ? 4 : 0;
  if (!n || left < n) return 0;
  uint32_t c = s[0] & (n == 1 ? 0x7f : (1u << (7 - n)) - 1);
  for (unsigned i = 1; i < n; ++i) {
    if ((s[i] & 0xc0) != 0x80) return 0;
    c = (c << 6) | (s[i] & 0x3f);
  }
  if ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) || (n == 4 && c < 0x10000) ||
      (c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff) return 0;
  *code = c;
  return n;
}
static int utf8_valid(fio_str_info_s s) {
  for (size_t i = 0, n; i < s.len; i += n) {
    uint32_t c;
    if (!(n = utf8_step(s.buf + i, s.len - i, &c))) return 0;
  }
  return 1;
}
static int js_space(uint32_t c) {
  return (c >= 9 && c <= 13) || c == 0x20 || c == 0xa0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
         c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}
/* Trim like JavaScript String.trim, returning codepoint count or -1 on bad UTF-8. */
static int utf8_trim(fio_str_info_s *s) {
  size_t start = 0, end = 0;
  int count = 0, kept = 0, started = 0;
  for (size_t i = 0, n; i < s->len; i += n) {
    uint32_t c;
    if (!(n = utf8_step(s->buf + i, s->len - i, &c))) return -1;
    if (!js_space(c)) {
      if (!started) { start = i; started = 1; }
      end = i + n;
      kept = ++count;
    } else if (started) ++count;
  }
  s->buf += start;
  s->len = end - start;
  return kept;
}
