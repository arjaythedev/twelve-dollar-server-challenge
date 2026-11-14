/* GENERATED phase-specific framing storage. No borrowed receive pointers. */
enum { FRAME_HEAD, FRAME_BODY };
typedef struct {
  u32 phase;
  union {
    struct { u32 scanned; } head;
    struct { u8 method; u8 close; u8 has_auth; u32 path_off; u32 path_len; u32 auth_off; u32 auth_len; u32 head_len; u32 body_len; } body;
  };
} frame_state;
_Static_assert(sizeof(frame_state) == 32, "cold frame layout");
static void frame_save_body(frame_state *fs, const req_t *r, const u8 *p, u32 he, u32 clen) {
  fs->body.method = r->method;
  fs->body.close = r->close;
  fs->body.has_auth = r->has_auth;
  fs->body.path_off = (u32)(r->path - p);
  fs->body.path_len = r->path_len;
  fs->body.auth_off = r->has_auth ? (u32)(r->auth - p) : 0;
  fs->body.auth_len = r->auth_len;
  fs->body.head_len = he;
  fs->body.body_len = clen;
  fs->phase = FRAME_BODY;
}
static void frame_restore_body(const frame_state *fs, req_t *r, const u8 *p) {
  r->method = fs->body.method;
  r->close = fs->body.close;
  r->has_auth = fs->body.has_auth;
  r->path = p + fs->body.path_off;
  r->path_len = fs->body.path_len;
  r->auth = fs->body.has_auth ? p + fs->body.auth_off : 0;
  r->auth_len = fs->body.auth_len;
  r->body = p + fs->body.head_len;
  r->body_len = fs->body.body_len;
}
