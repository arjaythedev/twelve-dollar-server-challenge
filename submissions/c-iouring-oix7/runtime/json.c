/* Validating JSON scanner and UTF-8 string unescaper for request bodies and
 * JWT payload segments. */

static u32 json_unescape(const u8 *s, u32 n, u8 *dst);

enum { JS_MAX_DEPTH = 64 };

typedef struct { const u8 *p, *end; int depth; } jscan;

/* Skip JSON whitespace (space, tab, LF, CR). */
static void js_ws(jscan *s) {
  while (s->p < s->end) {
    u8 c = *s->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') s->p++;
    else break;
  }
}

/* Hex digit value, or -1. */
static int hexv(u8 c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Length of the UTF-8 sequence at p (1..4), or 0 if invalid. Rejects
 * continuation bytes used as leads, overlong encodings, encoded surrogates
 * (0xED 0xA0..0xBF) and anything beyond U+10FFFF (0xF5..0xFF, or 0xF4 with a
 * trailing byte above 0x8F). */
static u32 utf8_len(const u8 *p, const u8 *end) {
  u8 c = p[0];
  if (c < 0x80) return 1;
  if (c < 0xC2) return 0; /* 0x80..0xC1: continuation byte or overlong lead */
  if (c < 0xE0) {         /* 0xC2..0xDF: U+0080..U+07FF */
    if (end - p < 2 || (p[1] & 0xC0) != 0x80) return 0;
    return 2;
  }
  if (c < 0xF0) {         /* 0xE0..0xEF: U+0800..U+FFFF */
    if (end - p < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0;               /* overlong */
    if (c == 0xED && (u8)(p[1] - 0xA0) <= 0x1F) return 0; /* U+D800..U+DFFF */
    return 3;
  }
  if (c < 0xF5) {         /* 0xF0..0xF4: U+10000..U+10FFFF */
    if (end - p < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
    if (c == 0xF0 && p[1] < 0x90) return 0; /* overlong */
    if (c == 0xF4 && p[1] > 0x8F) return 0; /* beyond U+10FFFF */
    return 4;
  }
  return 0; /* 0xF5..0xFF: invalid or beyond U+10FFFF */
}

/* Validate a JSON string at s->p, which must sit on the opening quote. On
 * success advance past the closing quote and set *v and *n to the raw bytes
 * between the quotes and *escaped to 1 if the content contains any escape.
 * Escapes: \uXXXX with 4 hex digits, or \" \\ \/ \b \f \n \r \t. Unescaped
 * bytes below 0x20 and invalid UTF-8 are rejected. Returns 0 or -1. */
static int js_string(jscan *s, const u8 **v, u32 *n, u8 *escaped) {
  const u8 *p = s->p;
  if (UNLIKELY(p >= s->end || *p != '"')) return -1;
  const u8 *start = ++p;
  u8 esc = 0;
  while (p < s->end) {
    u8 c = *p;
    if (c == '"') {
      s->p = p + 1;
      *v = start;
      *n = (u32)(p - start);
      *escaped = esc;
      return 0;
    }
    if (c == '\\') {
      esc = 1;
      if (UNLIKELY(++p >= s->end)) return -1;
      u8 e = *p++;
      if (e == 'u') {
        for (int i = 0; i < 4; i++) {
          if (UNLIKELY(p >= s->end || hexv(*p) < 0)) return -1;
          p++;
        }
      } else if (UNLIKELY(e != '"' && e != '\\' && e != '/' && e != 'b' &&
                          e != 'f' && e != 'n' && e != 'r' && e != 't')) {
        return -1;
      }
      continue;
    }
    if (UNLIKELY(c < 0x20)) return -1;
    u32 l = utf8_len(p, s->end);
    if (UNLIKELY(l == 0)) return -1;
    p += l;
  }
  return -1; /* unterminated */
}

/* Validate a strict JSON number at s->p:
 *   -? (0 | [1-9][0-9]*) ( '.' [0-9]+ )? ( [eE] [+-]? [0-9]+ )?
 * Advances past it. Returns 0 or -1. */
static int js_number(jscan *s) {
  const u8 *p = s->p;
  if (UNLIKELY(p >= s->end)) return -1;
  if (*p == '-') p++;
  if (UNLIKELY(p >= s->end)) return -1;
  if (*p == '0') {
    p++;
  } else if (*p >= '1' && *p <= '9') {
    do p++; while (p < s->end && *p >= '0' && *p <= '9');
  } else {
    return -1;
  }
  if (p < s->end && *p == '.') {
    if (UNLIKELY(++p >= s->end || *p < '0' || *p > '9')) return -1;
    while (p < s->end && *p >= '0' && *p <= '9') p++;
  }
  if (p < s->end && (*p == 'e' || *p == 'E')) {
    if (++p < s->end && (*p == '+' || *p == '-')) p++;
    if (UNLIKELY(p >= s->end || *p < '0' || *p > '9')) return -1;
    while (p < s->end && *p >= '0' && *p <= '9') p++;
  }
  s->p = p;
  return 0;
}

/* Validate true / false / null at s->p. Returns 0 or -1. */
static int js_lit(jscan *s) {
  const u8 *p = s->p;
  switch (*p) {
    case 't':
      if (s->end - p >= 4 && memcmp(p, "true", 4) == 0) { s->p = p + 4; return 0; }
      break;
    case 'f':
      if (s->end - p >= 5 && memcmp(p, "false", 5) == 0) { s->p = p + 5; return 0; }
      break;
    case 'n':
      if (s->end - p >= 4 && memcmp(p, "null", 4) == 0) { s->p = p + 4; return 0; }
      break;
  }
  return -1;
}

static int js_value(jscan *s);

/* Validate an array or object at s->p; nesting is limited to JS_MAX_DEPTH
 * levels. Returns 0 or -1. */
static int js_container(jscan *s) {
  u8 open = *s->p;
  u8 close = open == '[' ? ']' : '}';
  if (UNLIKELY(++s->depth > JS_MAX_DEPTH)) return -1;
  s->p++;
  js_ws(s);
  if (s->p < s->end && *s->p == close) {
    s->p++;
    s->depth--;
    return 0;
  }
  for (;;) {
    js_ws(s);
    if (open == '{') {
      const u8 *kv; u32 kn; u8 kesc;
      if (UNLIKELY(js_string(s, &kv, &kn, &kesc))) return -1;
      js_ws(s);
      if (UNLIKELY(s->p >= s->end || *s->p != ':')) return -1;
      s->p++;
    }
    if (UNLIKELY(js_value(s))) return -1;
    js_ws(s);
    if (UNLIKELY(s->p >= s->end)) return -1;
    if (*s->p == ',') { s->p++; continue; }
    if (*s->p == close) { s->p++; s->depth--; return 0; }
    return -1;
  }
}

static int js_value(jscan *s) {
  js_ws(s);
  if (UNLIKELY(s->p >= s->end)) return -1;
  u8 c = *s->p;
  if (c == '"') {
    const u8 *v; u32 n; u8 e;
    return js_string(s, &v, &n, &e);
  }
  if (c == '[' || c == '{') return js_container(s);
  if (c == 't' || c == 'f' || c == 'n') return js_lit(s);
  if (c == '-' || (c >= '0' && c <= '9')) return js_number(s);
  return -1;
}

/* Encode code point cp as UTF-8 (1..4 bytes); returns d past what it wrote. */
static inline u8 *put_utf8(u8 *d, u32 cp) {
  if (cp < 0x80) {
    *d++ = (u8)cp;
  } else if (cp < 0x800) {
    *d++ = (u8)(0xC0 | (cp >> 6));
    *d++ = (u8)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    *d++ = (u8)(0xE0 | (cp >> 12));
    *d++ = (u8)(0x80 | ((cp >> 6) & 0x3F));
    *d++ = (u8)(0x80 | (cp & 0x3F));
  } else {
    *d++ = (u8)(0xF0 | (cp >> 18));
    *d++ = (u8)(0x80 | ((cp >> 12) & 0x3F));
    *d++ = (u8)(0x80 | ((cp >> 6) & 0x3F));
    *d++ = (u8)(0x80 | (cp & 0x3F));
  }
  return d;
}

/* Decode the escapes of a JSON string's raw content s[0..n) (the bytes between
 * the quotes) into dst, which must have room for at least n bytes: the decoded
 * result is never longer than the raw input. Simple escapes expand; \uXXXX
 * becomes its code point; a high surrogate 0xD800..0xDBFF directly followed by
 * a low surrogate 0xDC00..0xDFFF combines into one code point; a lone
 * surrogate becomes U+FFFD. Returns the decoded length in bytes. */
static u32 json_unescape(const u8 *s, u32 n, u8 *dst) {
  u8 *d = dst;
  const u8 *p = s, *e = s + n;
  while (p < e) {
    u8 c = *p++;
    if (LIKELY(c != '\\')) { *d++ = c; continue; }
    if (UNLIKELY(p >= e)) { *d++ = '?'; break; } /* unreachable: validated input */
    u8 esc = *p++;
    switch (esc) {
      case '"':  *d++ = '"';  break;
      case '\\': *d++ = '\\'; break;
      case '/':  *d++ = '/';  break;
      case 'b':  *d++ = '\b'; break;
      case 'f':  *d++ = '\f'; break;
      case 'n':  *d++ = '\n'; break;
      case 'r':  *d++ = '\r'; break;
      case 't':  *d++ = '\t'; break;
      case 'u': {
        u32 cp = 0;
        int ok = 1;
        for (int i = 0; i < 4; i++) {
          if (UNLIKELY(p >= e)) { ok = 0; break; }
          int h = hexv(*p++);
          if (UNLIKELY(h < 0)) { ok = 0; break; }
          cp = (cp << 4) | (u32)h;
        }
        if (UNLIKELY(!ok)) { *d++ = '?'; break; } /* unreachable: validated */
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            u32 lo = 0;
            int ok2 = 1;
            for (int i = 0; i < 4; i++) {
              int h = hexv(p[2 + i]);
              if (h < 0) { ok2 = 0; break; }
              lo = (lo << 4) | (u32)h;
            }
            if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
              p += 6;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              d = put_utf8(d, cp);
              break;
            }
          }
          d = put_utf8(d, 0xFFFD); /* lone high surrogate */
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          d = put_utf8(d, 0xFFFD); /* lone low surrogate */
        } else {
          d = put_utf8(d, cp);
        }
        break;
      }
      default:
        *d++ = '?'; /* unreachable: validated input */
        break;
    }
  }
  return (u32)(d - dst);
}

/* Validate the whole JSON document [p, p+len) and match its top-level object
 * keys against m[0..nm-1]. Returns -1 if the document is not valid JSON, 1 if
 * it is valid but the root is not an object, and 0 if the root is a valid
 * object. On 0, each m[i] whose .key matches (last occurrence wins, like
 * JSON.parse) is filled with the raw value slice: .type (JT_STRING, JT_NUMBER
 * or JT_OTHER), .v/.v_len and .escaped. For JT_STRING, .v points at the bytes
 * between the quotes and .escaped is 1 when json_unescape(.v, .v_len, dst)
 * must be run to obtain the decoded text (dst needs at least .v_len bytes;
 * the decoded result is never longer). Unmatched entries keep JT_NONE.
 * Escaped keys are unescaped into a 64-byte scratch buffer; a decoded key is
 * never longer than its raw form, so keys of up to 64 raw bytes always fit. */
static int json_members(const u8 *p, u32 len, jmember *m, int nm) {
  for (int i = 0; i < nm; i++) m[i].type = JT_NONE;

  jscan s = { p, p + len, 0 };
  js_ws(&s);
  if (UNLIKELY(s.p >= s.end)) return -1;

  if (*s.p != '{') {
    /* Valid JSON with a non-object root: require the whole document to parse. */
    if (UNLIKELY(js_value(&s))) return -1;
    js_ws(&s);
    return s.p == s.end ? 1 : -1;
  }
  s.p++;
  s.depth = 1; /* the root object counts as the first nesting level */

  js_ws(&s);
  if (s.p < s.end && *s.p == '}') {
    s.p++;
  } else {
    for (;;) {
      const u8 *kv, *v0, *vv; u32 kn, vn; u8 kesc, vesc;
      js_ws(&s);
      if (UNLIKELY(js_string(&s, &kv, &kn, &kesc))) return -1;
      js_ws(&s);
      if (UNLIKELY(s.p >= s.end || *s.p != ':')) return -1;
      s.p++;
      js_ws(&s);
      if (UNLIKELY(s.p >= s.end)) return -1;
      v0 = s.p;
      u8 t = JT_OTHER;
      vv = v0;
      vn = 0;
      vesc = 0;
      if (*v0 == '"') {
        if (UNLIKELY(js_string(&s, &vv, &vn, &vesc))) return -1;
        t = JT_STRING;
      } else {
        if (UNLIKELY(js_value(&s))) return -1;
        vn = (u32)(s.p - v0);
        if (*v0 == '-' || (*v0 >= '0' && *v0 <= '9')) t = JT_NUMBER;
      }

      /* Match the key; unescape escaped keys into a 64-byte scratch buffer. */
      u8 kbuf[64];
      const u8 *kk;
      u32 klen;
      if (kesc) {
        if (kn <= sizeof kbuf) { klen = json_unescape(kv, kn, kbuf); kk = kbuf; }
        else { kk = kbuf; klen = 0; } /* too long to unescape here: never matches */
      } else {
        kk = kv; klen = kn;
      }
      for (int i = 0; i < nm; i++) {
        if (klen != 0 && m[i].key_len == klen && memcmp(m[i].key, kk, klen) == 0) {
          m[i].type = t;
          m[i].v = vv;
          m[i].v_len = vn;
          m[i].escaped = vesc;
        }
      }

      js_ws(&s);
      if (UNLIKELY(s.p >= s.end)) return -1;
      if (*s.p == ',') { s.p++; continue; }
      if (*s.p == '}') { s.p++; break; }
      return -1;
    }
  }

  js_ws(&s);
  return s.p == s.end ? 0 : -1;
}