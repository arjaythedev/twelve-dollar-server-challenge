#include "http.h"
#include <string.h>

static int ci_eq(const char *a, size_t n, const char *lit) {
  size_t i = 0;
  for (; lit[i]; i++) {
    if (i >= n) return 0;
    char ca = a[i], cb = lit[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
    if (ca != cb) return 0;
  }
  return i == n;
}

// Find end of headers (\r\n\r\n). Returns offset just past it, or 0 if incomplete.
static size_t hdr_end(const char *b, size_t n) {
  for (size_t i = 0; i + 3 < n; i++) {
    if (b[i] == '\r' && b[i+1] == '\n' && b[i+2] == '\r' && b[i+3] == '\n') return i + 4;
  }
  // tolerate bare \n\n? No: strict CRLF only (all our clients speak CRLF).
  return 0;
}

static const char *find_line_end(const char *p, const char *e) {
  for (; p + 1 < e; p++) {
    if (p[0] == '\r' && p[1] == '\n') return p;
  }
  return NULL;
}

// Parse hex chunk-size (ignore extensions after ';'). -1 on error.
static long chunk_size(const char *p, const char *e) {
  long v = 0;
  int digits = 0;
  while (p < e && *p != '\r' && *p != ';') {
    char ch = *p++;
    v <<= 4;
    if (ch >= '0' && ch <= '9') v |= ch - '0';
    else if (ch >= 'a' && ch <= 'f') v |= ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F') v |= ch - 'A' + 10;
    else return -1;
    digits++;
    if (v > (long)HTTP_MAX_TOTAL) return -1;
  }
  if (digits == 0) return -1;
  return v;
}

int http_parse(char *buf, size_t len, req_t *out) {
  if (len > HTTP_MAX_TOTAL + 8192) return -2; // already absurd; close early
  size_t he = hdr_end(buf, len);
  if (he == 0) {
    if (len >= HTTP_MAX_TOTAL) return -2;
    return 0;
  }
  const char *e = buf + len;
  const char *p = buf;

  // --- request line ---
  const char *le = find_line_end(p, buf + he);
  if (!le) return -1;
  const char *sp1 = NULL, *sp2 = NULL;
  for (const char *q = p; q < le; q++) {
    if (*q == ' ') { if (!sp1) sp1 = q; else { sp2 = q; break; } }
  }
  if (!sp1 || !sp2) return -1;
  // method
  out->method = p; out->mlen = (size_t)(sp1 - p);
  if (out->mlen == 0 || out->mlen > 8) return -1;
  // target: strip query
  const char *ts = sp1 + 1, *te = sp2;
  if (ts >= te) return -1;
  const char *qm = NULL;
  for (const char *q = ts; q < te; q++) if (*q == '?') { qm = q; break; }
  out->path = ts; out->plen = (size_t)((qm ? qm : te) - ts);
  if (out->plen == 0 || out->path[0] != '/') return -1;
  // version must be HTTP/1.x
  size_t vlen = (size_t)(le - (sp2 + 1));
  if (vlen != 8 || memcmp(sp2 + 1, "HTTP/1.", 7) != 0) return -1;
  {
    char minor = *(sp2 + 8);
    if (minor != '0' && minor != '1') return -1;
  }

  // --- headers ---
  out->auth = NULL; out->auth_len = 0;
  out->conn_close = 0; out->expect_continue = 0;
  long content_length = -1;
  int chunked = 0;
  p = le + 2;
  const char *hend = buf + he;
  while (p < hend - 2) {
    le = find_line_end(p, hend);
    if (!le) return -1;
    const char *colon = NULL;
    for (const char *q = p; q < le; q++) if (*q == ':') { colon = q; break; }
    if (!colon) return -1;
    // trim OWS around value
    const char *vs = colon + 1, *ve = le;
    while (vs < ve && (*vs == ' ' || *vs == '\t')) vs++;
    while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
    size_t nlen = (size_t)(colon - p);
    if (ci_eq(p, nlen, "content-length")) {
      if (content_length >= 0) return -1; // duplicate C-L: ambiguous, close
      long v = 0;
      if (vs >= ve) return -1;
      for (const char *q = vs; q < ve; q++) {
        if (*q < '0' || *q > '9') return -1;
        v = v * 10 + (*q - '0');
        if (v > (long)(HTTP_MAX_TOTAL + 4096)) return -2;
      }
      content_length = v;
    } else if (ci_eq(p, nlen, "transfer-encoding")) {
      // last coding must be chunked; anything else we don't support
      const char *tok = ve;
      while (tok > vs && *(tok-1) != ',') tok--;
      while (tok < ve && (*tok == ' ' || *tok == '\t')) tok++;
      size_t tl = (size_t)(ve - tok);
      while (tl && (tok[tl-1] == ' ' || tok[tl-1] == '\t')) tl--;
      if (!ci_eq(tok, tl, "chunked")) return -1;
      chunked = 1;
    } else if (ci_eq(p, nlen, "authorization")) {
      if (!out->auth) { out->auth = vs; out->auth_len = (size_t)(ve - vs); }
    } else if (ci_eq(p, nlen, "connection")) {
      // token search for "close"
      const char *q = vs;
      while (q < ve) {
        while (q < ve && (*q == ' ' || *q == '\t' || *q == ',')) q++;
        if (q + 5 <= ve && ci_eq(q, 5, "close") &&
            (q + 5 == ve || q[5] == ',' || q[5] == ' ' || q[5] == '\t')) { out->conn_close = 1; break; }
        while (q < ve && *q != ',') q++;
      }
    } else if (ci_eq(p, nlen, "expect")) {
      if (ci_eq(vs, (size_t)(ve - vs), "100-continue")) out->expect_continue = 1;
    }
    p = le + 2;
  }

  // --- body ---
  // Header fields are final now; publish them so a 0 (need-more-data) return
  // still tells the caller about Expect: 100-continue etc.
  out->body = NULL; out->body_len = 0;
  out->headers_done = 1;
  if (chunked) {
    // compact chunks in place starting at he
    size_t rp = he, wp = he;
    for (;;) {
      const char *cle = find_line_end(buf + rp, e);
      if (!cle) return 0; // need more
      long sz = chunk_size(buf + rp, cle);
      if (sz < 0) return -1;
      rp = (size_t)(cle - buf) + 2;
      if (sz == 0) {
        // trailers until empty line
        for (;;) {
          cle = find_line_end(buf + rp, e);
          if (!cle) return 0;
          rp = (size_t)(cle - buf) + 2;
          if (cle == buf + rp - 2) break; // empty line
          if (rp > he + HTTP_MAX_TOTAL) return -2;
        }
        out->body = buf + he; out->body_len = wp - he;
        out->consumed = rp;
        if (out->body_len > HTTP_MAX_TOTAL) return -2;
        return 1;
      }
      if (rp + (size_t)sz + 2 > len) {
        if (he + (size_t)sz > HTTP_MAX_TOTAL) return -2;
        return 0;
      }
      if (wp != rp) memmove(buf + wp, buf + rp, (size_t)sz);
      rp += (size_t)sz; wp += (size_t)sz;
      if (buf[rp] != '\r' || buf[rp+1] != '\n') return -1;
      rp += 2;
    }
  }
  if (content_length < 0) content_length = 0;
  if (he + (size_t)content_length > len) {
    if (he + (size_t)content_length > HTTP_MAX_TOTAL) return -2;
    return 0;
  }
  out->body = buf + he; out->body_len = (size_t)content_length;
  out->consumed = he + (size_t)content_length;
  return 1;
}
