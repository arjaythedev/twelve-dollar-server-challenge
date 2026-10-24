// parser/cmp/uw_uws.cpp (sha256 d8b0994aff6d), developed separately for this entry.
// uWebSockets (vendor/uws, v20.80.0) HTTP/1.1 parser behind picohttpparser's phr_parse_request signature.
// uWS's parser proper is HttpParser::getHeaders (private static, reached with the usual #define private
// public). It needs a WRITABLE post-padded buffer: like fenceAndConsumePostPadded we fence buf[len] = '\r',
// buf[len+1] = 'a' (inside the caller's >= 64 B padding), and it lowercases header names in place.
// Not included (that is uWS's request handling around the parser): Host-required check, bloom filter,
// Content-Length / chunked handling, query split. Our shared logic does the CL part.
#include <cstddef>
#define private public
#include "../uws/src/HttpParser.h"
#undef private
extern "C" {
#include "../pico/picohttpparser.h"

static inline __attribute__((always_inline)) int uw_core(const char *buf, size_t len, const char **method, size_t *method_len,
    const char **path, size_t *path_len, int *minor, struct phr_header *headers, size_t *num_headers, int fence) {
  char *data = (char *)buf;
  if (fence) { data[len] = '\r'; data[len + 1] = 'a'; }
  uWS::HttpRequest::Header h[UWS_HTTP_MAX_HEADERS_COUNT];
  unsigned int err = 0;
  unsigned int used = uWS::HttpParser::getHeaders(data, data + len, h, nullptr, err);
  if (!used) return err ? -1 : -2;
  *method = h[0].key.data(); *method_len = h[0].key.size();
  *path = h[0].value.data(); *path_len = h[0].value.size();
  *minor = 1;                                   // getHeaders only accepts " HTTP/1.1\r\n"
  size_t n = 0, max = *num_headers;
  for (unsigned int i = 1; h[i].key.data() && n < max; i++, n++) {
    headers[n].name = h[i].key.data(); headers[n].name_len = h[i].key.size();
    headers[n].value = h[i].value.data(); headers[n].value_len = h[i].value.size();
  }
  *num_headers = n;
  return (int)used;
}
// as uWS does it: fence on every call
int uw_parse_request(const char *buf, size_t len, const char **method, size_t *method_len, const char **path, size_t *path_len,
                     int *minor, struct phr_header *headers, size_t *num_headers, size_t last_len) {
  (void)last_len; return uw_core(buf, len, method, method_len, path, path_len, minor, headers, num_headers, 1);
}
// fence already written by the caller (once per receive buffer): buf[len] == '\r', buf[len+1] == 'a'
int uw_parse_request_prefenced(const char *buf, size_t len, const char **method, size_t *method_len, const char **path,
                     size_t *path_len, int *minor, struct phr_header *headers, size_t *num_headers, size_t last_len) {
  (void)last_len; return uw_core(buf, len, method, method_len, path, path_len, minor, headers, num_headers, 0);
}
}
