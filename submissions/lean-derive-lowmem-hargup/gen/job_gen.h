/* GENERATED from Endpoint.payload. */
typedef struct { i64 id; } args_post;
typedef struct { i64 id; i64 uid; } args_like;
typedef struct { i64 uid; const u8 *name; u32 name_len; const u8 *text; u32 text_len; u8 text_plain; } args_create;
typedef struct job {
  u8 kind, is_write, close_after;
  u32 conn, gen, next;
  void *payload;
  obuf *resp;
} job_t;
_Static_assert(sizeof(job_t) == 32, "compact job envelope");
