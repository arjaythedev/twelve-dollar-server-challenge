// parser/cmp/lx_llhttp.c (sha256 6cad08c050dc), developed separately for this entry.
// llhttp (vendor/llhttp, v9.4.3, Node.js's parser) behind picohttpparser's phr_parse_request
// signature: callbacks record the URL and header spans, on_headers_complete pauses the parser,
// and the pause position is the header length (what pico returns). The body is left to the logic.
#include <stddef.h>
#include "../llhttp/include/llhttp.h"
#include "../../cursed-http-parser/vendor/pico/picohttpparser.h"

typedef struct { llhttp_t p; const char *url; size_t url_len; struct phr_header *h; size_t nh, max; int field_open, overflow; } lx_t;

static int on_url(llhttp_t *p, const char *at, size_t n) { lx_t *s = (lx_t *)p; if (!s->url) s->url = at; s->url_len += n; return 0; }
static int on_field(llhttp_t *p, const char *at, size_t n) {
  lx_t *s = (lx_t *)p;
  if (!s->field_open) { if (s->nh == s->max) { s->overflow = 1; return HPE_USER; } s->h[s->nh].name = at; s->h[s->nh].name_len = 0;
    s->h[s->nh].value = at; s->h[s->nh].value_len = 0; s->field_open = 1; }
  s->h[s->nh].name_len += n; return 0;
}
static int on_value(llhttp_t *p, const char *at, size_t n) {
  lx_t *s = (lx_t *)p; struct phr_header *h = &s->h[s->nh];
  if (!h->value_len) h->value = at;
  h->value_len += n; return 0;
}
static int on_value_done(llhttp_t *p) { lx_t *s = (lx_t *)p; s->nh++; s->field_open = 0; return 0; }
static int on_headers_done(llhttp_t *p) { (void)p; return HPE_PAUSED; }

static llhttp_settings_t SETTINGS; static int settings_ready;

int lx_parse_request(const char *buf, size_t len, const char **method, size_t *method_len, const char **path, size_t *path_len,
                     int *minor, struct phr_header *headers, size_t *num_headers, size_t last_len) {
  (void)last_len;
  if (!settings_ready) { llhttp_settings_init(&SETTINGS); SETTINGS.on_url = on_url; SETTINGS.on_header_field = on_field;
    SETTINGS.on_header_value = on_value; SETTINGS.on_header_value_complete = on_value_done; SETTINGS.on_headers_complete = on_headers_done; settings_ready = 1; }
  lx_t s; llhttp_init(&s.p, HTTP_REQUEST, &SETTINGS);
  s.url = 0; s.url_len = 0; s.h = headers; s.nh = 0; s.max = *num_headers; s.field_open = 0; s.overflow = 0;
  llhttp_errno_t e = llhttp_execute(&s.p, buf, len);
  if (e == HPE_OK) return -2;                     // consumed everything without reaching the end of headers
  if (e != HPE_PAUSED) return -1;
  const char *m = llhttp_method_name((llhttp_method_t)s.p.method);
  *method = m; *method_len = 0; while (m[*method_len]) ++*method_len;
  *path = s.url; *path_len = s.url_len; *minor = s.p.http_major == 1 ? s.p.http_minor : -1;
  for (size_t i = 0; i < s.nh; i++) {           // llhttp hands over values with trailing OWS: trim like pico
    while (headers[i].value_len && (headers[i].value[headers[i].value_len - 1] == ' ' || headers[i].value[headers[i].value_len - 1] == '\t')) headers[i].value_len--;
  }
  *num_headers = s.nh;
  return (int)(llhttp_get_error_pos(&s.p) - buf);
}
